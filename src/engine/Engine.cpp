#include "engine/Engine.hpp"
#include "settings.hpp"
#include "system.hpp"
#include "random.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <x86intrin.h>


namespace rack {
namespace engine {


static void disableDenormals() {
	// Set CPU to flush-to-zero (FTZ) and denormals-are-zero (DAZ) mode
	// https://software.intel.com/en-us/node/682949
	_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
	_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}


/** Threads which obtain a VIPLock will cause wait() to block for other less important threads.
This does not provide the VIPs with an exclusive lock. That should be left up to another mutex shared between the less important thread.
*/
struct VIPMutex {
	int count = 0;
	std::condition_variable cv;
	std::mutex countMutex;

	/** Blocks until there are no remaining VIPLocks */
	void wait() {
		std::unique_lock<std::mutex> lock(countMutex);
		while (count > 0)
			cv.wait(lock);
	}
};


struct VIPLock {
	VIPMutex &m;
	VIPLock(VIPMutex &m) : m(m) {
		std::unique_lock<std::mutex> lock(m.countMutex);
		m.count++;
	}
	~VIPLock() {
		std::unique_lock<std::mutex> lock(m.countMutex);
		m.count--;
		lock.unlock();
		m.cv.notify_all();
	}
};


struct Barrier {
	std::mutex mutex;
	std::condition_variable cv;
	int count = 0;
	int total = 0;

	void wait() {
		// Waiting on one thread is trivial.
		if (total <= 1)
			return;
		std::unique_lock<std::mutex> lock(mutex);
		int id = ++count;
		if (id == total) {
			count = 0;
			cv.notify_all();
		}
		else {
			cv.wait(lock);
		}
	}
};


struct SpinBarrier {
	std::atomic<int> count{0};
	int total = 0;

	void wait() {
		int id = ++count;
		if (id == total) {
			count = 0;
		}
		else {
			while (count != 0) {
				_mm_pause();
			}
		}
	}
};


/** Spinlocks until all `total` threads are waiting.
If `yield` is set to true at any time, all threads will switch to waiting on a mutex instead.
All threads must return before beginning a new phase. Alternating between two barriers solves this problem.
*/
struct HybridBarrier {
	std::atomic<int> count {0};
	int total = 0;

	std::mutex mutex;
	std::condition_variable cv;

	std::atomic<bool> yield {false};

	void wait() {
		int id = ++count;

		// End and reset phase if this is the last thread
		if (id == total) {
			count = 0;
			if (yield) {
				std::unique_lock<std::mutex> lock(mutex);
				cv.notify_all();
				yield = false;
			}
			return;
		}

		// Spinlock
		while (!yield) {
			if (count == 0)
				return;
			_mm_pause();
		}

		// Wait on mutex
		{
			std::unique_lock<std::mutex> lock(mutex);
			cv.wait(lock, [&]{
				return count == 0;
			});
		}
	}
};


struct EngineWorker {
	Engine *engine;
	int id;
	std::thread thread;
	bool running = true;

	void start() {
		thread = std::thread([&] {
			random::init();
			run();
		});
	}

	void stop() {
		running = false;
	}

	void join() {
		thread.join();
	}

	void run();
};


struct Engine::Internal {
	std::vector<Module*> modules;
	std::vector<Cable*> cables;
	std::vector<ParamHandle*> paramHandles;
	bool paused = false;

	bool running = false;
	float sampleRate;
	float sampleTime;

	int nextModuleId = 0;
	int nextCableId = 0;

	// Parameter smoothing
	Module *smoothModule = NULL;
	int smoothParamId;
	float smoothValue;

	std::recursive_mutex mutex;
	std::thread thread;
	VIPMutex vipMutex;

	bool realTime = false;
	int threadCount = 1;
	std::vector<EngineWorker> workers;
	HybridBarrier engineBarrier;
	HybridBarrier workerBarrier;
	std::atomic<int> workerModuleIndex;
};


Engine::Engine() {
	internal = new Internal;

	internal->engineBarrier.total = 1;
	internal->workerBarrier.total = 1;

	internal->sampleRate = 44100.f;
	internal->sampleTime = 1 / internal->sampleRate;

	system::setThreadRealTime(false);
}

Engine::~Engine() {
	// Make sure there are no cables or modules in the rack on destruction.
	// If this happens, a module must have failed to remove itself before the RackWidget was destroyed.
	assert(internal->cables.empty());
	assert(internal->modules.empty());
	assert(internal->paramHandles.empty());

	delete internal;
}

static void Engine_stepModules(Engine *that, int threadId) {
	Engine::Internal *internal = that->internal;

	// int threadCount = internal->threadCount;
	int modulesLen = internal->modules.size();
	float sampleTime = internal->sampleTime;

	Module::ProcessArgs processCtx;
	processCtx.sampleRate = internal->sampleRate;
	processCtx.sampleTime = internal->sampleTime;

	// Step each module
	// for (int i = threadId; i < modulesLen; i += threadCount) {
	while (true) {
		// Choose next module
		int i = internal->workerModuleIndex++;
		if (i >= modulesLen)
			break;

		Module *module = internal->modules[i];
		if (!module->bypass) {
			// Step module
			if (settings::cpuMeter) {
				auto startTime = std::chrono::high_resolution_clock::now();

				module->process(processCtx);

				auto stopTime = std::chrono::high_resolution_clock::now();
				float cpuTime = std::chrono::duration<float>(stopTime - startTime).count();
				// Smooth CPU time
				const float cpuTau = 2.f /* seconds */;
				module->cpuTime += (cpuTime - module->cpuTime) * sampleTime / cpuTau;
			}
			else {
				module->process(processCtx);
			}
		}

		// Iterate ports to step plug lights
		for (Input &input : module->inputs) {
			input.process(sampleTime);
		}
		for (Output &output : module->outputs) {
			output.process(sampleTime);
		}
	}
}

static void Engine_step(Engine *that) {
	Engine::Internal *internal = that->internal;

	// Param smoothing
	Module *smoothModule = internal->smoothModule;
	int smoothParamId = internal->smoothParamId;
	float smoothValue = internal->smoothValue;
	if (smoothModule) {
		Param *param = &smoothModule->params[smoothParamId];
		float value = param->value;
		// Decay rate is 1 graphics frame
		const float smoothLambda = 60.f;
		float newValue = value + (smoothValue - value) * smoothLambda * internal->sampleTime;
		if (value == newValue) {
			// Snap to actual smooth value if the value doesn't change enough (due to the granularity of floats)
			param->setValue(smoothValue);
			internal->smoothModule = NULL;
			internal->smoothParamId = 0;
		}
		else {
			param->value = newValue;
		}
	}

	// Step modules along with workers
	internal->workerModuleIndex = 0;
	internal->engineBarrier.wait();
	Engine_stepModules(that, 0);
	internal->workerBarrier.wait();

	// Step cables
	for (Cable *cable : that->internal->cables) {
		cable->step();
	}

	// Flip messages for each module
	for (Module *module : that->internal->modules) {
		if (module->leftExpander.messageFlipRequested) {
			std::swap(module->leftExpander.producerMessage, module->leftExpander.consumerMessage);
			module->leftExpander.messageFlipRequested = false;
		}
		if (module->rightExpander.messageFlipRequested) {
			std::swap(module->rightExpander.producerMessage, module->rightExpander.consumerMessage);
			module->rightExpander.messageFlipRequested = false;
		}
	}
}

static void Engine_updateExpander(Engine *that, Module::Expander *expander) {
	if (expander->moduleId >= 0) {
		if (!expander->module || expander->module->id != expander->moduleId) {
			expander->module = that->getModule(expander->moduleId);
		}
	}
	else {
		if (expander->module) {
			expander->module = NULL;
		}
	}
}

static void Engine_relaunchWorkers(Engine *that) {
	Engine::Internal *internal = that->internal;
	assert(1 <= internal->threadCount);

	// Stop all workers
	for (EngineWorker &worker : internal->workers) {
		worker.stop();
	}
	internal->engineBarrier.wait();

	// Destroy all workers
	for (EngineWorker &worker : internal->workers) {
		worker.join();
	}
	internal->workers.resize(0);

	// Configure main thread
	system::setThreadRealTime(internal->realTime);

	// Set barrier counts
	internal->engineBarrier.total = internal->threadCount;
	internal->workerBarrier.total = internal->threadCount;

	// Create workers
	internal->workers.resize(internal->threadCount - 1);
	for (int id = 1; id < internal->threadCount; id++) {
		EngineWorker &worker = internal->workers[id - 1];
		worker.id = id;
		worker.engine = that;
		worker.start();
	}
}

static void Engine_run(Engine *that) {
	Engine::Internal *internal = that->internal;
	// Set up thread
	system::setThreadName("Engine");
	// system::setThreadRealTime();
	disableDenormals();

	// Every time the that waits and locks a mutex, it steps this many frames
	const int mutexSteps = 128;
	// Time in seconds that the that is rushing ahead of the estimated clock time
	double aheadTime = 0.0;
	auto lastTime = std::chrono::high_resolution_clock::now();

	while (internal->running) {
		internal->vipMutex.wait();

		// Set sample rate
		if (internal->sampleRate != settings::sampleRate) {
			internal->sampleRate = settings::sampleRate;
			internal->sampleTime = 1 / internal->sampleRate;
			for (Module *module : internal->modules) {
				module->onSampleRateChange();
			}
			aheadTime = 0.0;
		}

		// Launch workers
		if (internal->threadCount != settings::threadCount || internal->realTime != settings::realTime) {
			internal->threadCount = settings::threadCount;
			internal->realTime = settings::realTime;
			Engine_relaunchWorkers(that);
		}

		if (!internal->paused) {
			std::lock_guard<std::recursive_mutex> lock(internal->mutex);

			// Update expander pointers
			for (Module *module : internal->modules) {
				Engine_updateExpander(that, &module->leftExpander);
				Engine_updateExpander(that, &module->rightExpander);
			}

			// Step modules
			for (int i = 0; i < mutexSteps; i++) {
				Engine_step(that);
			}
		}

		double stepTime = mutexSteps * internal->sampleTime;
		aheadTime += stepTime;
		auto currTime = std::chrono::high_resolution_clock::now();
		const double aheadFactor = 2.0;
		aheadTime -= aheadFactor * std::chrono::duration<double>(currTime - lastTime).count();
		lastTime = currTime;
		aheadTime = std::fmax(aheadTime, 0.0);

		// Avoid pegging the CPU at 100% when there are no "blocking" modules like AudioInterface, but still step audio at a reasonable rate
		// The number of steps to wait before possibly sleeping
		const double aheadMax = 1.0; // seconds
		if (aheadTime > aheadMax) {
			std::this_thread::sleep_for(std::chrono::duration<double>(stepTime));
		}
	}

	// Stop workers
	internal->threadCount = 1;
	Engine_relaunchWorkers(that);
}

void Engine::start() {
	internal->running = true;
	internal->thread = std::thread([&] {
		random::init();
		Engine_run(this);
	});
}

void Engine::stop() {
	internal->running = false;
	internal->thread.join();
}

void Engine::setPaused(bool paused) {
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);
	internal->paused = paused;
}

bool Engine::isPaused() {
	// No lock
	return internal->paused;
}

float Engine::getSampleRate() {
	return internal->sampleRate;
}

float Engine::getSampleTime() {
	return internal->sampleTime;
}

void Engine::yieldWorkers() {
	internal->workerBarrier.yield = true;
}

void Engine::addModule(Module *module) {
	assert(module);
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);
	// Check that the module is not already added
	auto it = std::find(internal->modules.begin(), internal->modules.end(), module);
	assert(it == internal->modules.end());
	// Set ID
	if (module->id < 0) {
		// Automatically assign ID
		module->id = internal->nextModuleId++;
	}
	else {
		// Manual ID
		// Check that the ID is not already taken
		for (Module *m : internal->modules) {
			assert(module->id != m->id);
		}
		if (module->id >= internal->nextModuleId) {
			internal->nextModuleId = module->id + 1;
		}
	}
	// Add module
	internal->modules.push_back(module);
	// Trigger Add event
	module->onAdd();
	// Update ParamHandles
	for (ParamHandle *paramHandle : internal->paramHandles) {
		if (paramHandle->moduleId == module->id)
			paramHandle->module = module;
	}
}

void Engine::removeModule(Module *module) {
	assert(module);
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);
	// Check that the module actually exists
	auto it = std::find(internal->modules.begin(), internal->modules.end(), module);
	assert(it != internal->modules.end());
	// If a param is being smoothed on this module, stop smoothing it immediately
	if (module == internal->smoothModule) {
		internal->smoothModule = NULL;
	}
	// Check that all cables are disconnected
	for (Cable *cable : internal->cables) {
		assert(cable->outputModule != module);
		assert(cable->inputModule != module);
	}
	// Update ParamHandles
	for (ParamHandle *paramHandle : internal->paramHandles) {
		if (paramHandle->moduleId == module->id)
			paramHandle->module = NULL;
	}
	// Update expander pointers
	for (Module *m : internal->modules) {
		if (m->leftExpander.module == module) {
			m->leftExpander.moduleId = -1;
			m->leftExpander.module = NULL;
		}
		if (m->rightExpander.module == module) {
			m->rightExpander.moduleId = -1;
			m->rightExpander.module = NULL;
		}
	}
	// Trigger Remove event
	module->onRemove();
	// Remove module
	internal->modules.erase(it);
}

Module *Engine::getModule(int moduleId) {
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);
	// Find module
	for (Module *module : internal->modules) {
		if (module->id == moduleId)
			return module;
	}
	return NULL;
}

void Engine::resetModule(Module *module) {
	assert(module);
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);

	module->onReset();
}

void Engine::randomizeModule(Module *module) {
	assert(module);
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);

	module->onRandomize();
}

void Engine::bypassModule(Module *module, bool bypass) {
	assert(module);
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);
	if (bypass) {
		for (Output &output : module->outputs) {
			// This also zeros all voltages
			output.setChannels(0);
		}
		module->cpuTime = 0.f;
	}
	else {
		// Set all outputs to 1 channel
		for (Output &output : module->outputs) {
			output.setChannels(1);
		}
	}
	module->bypass = bypass;
}

static void Engine_updateConnected(Engine *that) {
	// Set everything to unconnected
	for (Module *module : that->internal->modules) {
		for (Input &input : module->inputs) {
			input.active = false;
		}
		for (Output &output : module->outputs) {
			output.active = false;
		}
	}
	// Set inputs/outputs to active
	for (Cable *cable : that->internal->cables) {
		cable->outputModule->outputs[cable->outputId].active = true;
		cable->inputModule->inputs[cable->inputId].active = true;
	}
}

void Engine::addCable(Cable *cable) {
	assert(cable);
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);
	// Check cable properties
	assert(cable->outputModule);
	assert(cable->inputModule);
	// Check that the cable is not already added, and that the input is not already used by another cable
	for (Cable *cable2 : internal->cables) {
		assert(cable2 != cable);
		assert(!(cable2->inputModule == cable->inputModule && cable2->inputId == cable->inputId));
	}
	// Set ID
	if (cable->id < 0) {
		// Automatically assign ID
		cable->id = internal->nextCableId++;
	}
	else {
		// Manual ID
		// Check that the ID is not already taken
		for (Cable *w : internal->cables) {
			assert(cable->id != w->id);
		}
		if (cable->id >= internal->nextCableId) {
			internal->nextCableId = cable->id + 1;
		}
	}
	// Add the cable
	internal->cables.push_back(cable);
	Engine_updateConnected(this);
}

void Engine::removeCable(Cable *cable) {
	assert(cable);
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);
	// Check that the cable is already added
	auto it = std::find(internal->cables.begin(), internal->cables.end(), cable);
	assert(it != internal->cables.end());
	// Set input to inactive
	Input &input = cable->inputModule->inputs[cable->inputId];
	input.setChannels(0);
	// Remove the cable
	internal->cables.erase(it);
	Engine_updateConnected(this);
}

void Engine::setParam(Module *module, int paramId, float value) {
	// TODO Does this need to be thread-safe?
	// If being smoothed, cancel smoothing
	if (internal->smoothModule == module && internal->smoothParamId == paramId) {
		internal->smoothModule = NULL;
		internal->smoothParamId = 0;
	}
	module->params[paramId].value = value;
}

float Engine::getParam(Module *module, int paramId) {
	return module->params[paramId].value;
}

void Engine::setSmoothParam(Module *module, int paramId, float value) {
	// If another param is being smoothed, jump value
	if (internal->smoothModule && !(internal->smoothModule == module && internal->smoothParamId == paramId)) {
		internal->smoothModule->params[internal->smoothParamId].value = internal->smoothValue;
	}
	internal->smoothParamId = paramId;
	internal->smoothValue = value;
	// Set this last so the above values are valid as soon as it is set
	internal->smoothModule = module;
}

float Engine::getSmoothParam(Module *module, int paramId) {
	if (internal->smoothModule == module && internal->smoothParamId == paramId)
		return internal->smoothValue;
	return getParam(module, paramId);
}

void Engine::addParamHandle(ParamHandle *paramHandle) {
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);

	// Check that the ParamHandle is not already added
	auto it = std::find(internal->paramHandles.begin(), internal->paramHandles.end(), paramHandle);
	assert(it == internal->paramHandles.end());

	// New ParamHandles must be blank
	assert(paramHandle->moduleId < 0);
	internal->paramHandles.push_back(paramHandle);
}

void Engine::removeParamHandle(ParamHandle *paramHandle) {
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);

	paramHandle->module = NULL;
	// Check that the ParamHandle is already added
	auto it = std::find(internal->paramHandles.begin(), internal->paramHandles.end(), paramHandle);
	assert(it != internal->paramHandles.end());
	internal->paramHandles.erase(it);
}

ParamHandle *Engine::getParamHandle(Module *module, int paramId) {
	// VIPLock vipLock(internal->vipMutex);
	// std::lock_guard<std::recursive_mutex> lock(internal->mutex);

	for (ParamHandle *paramHandle : internal->paramHandles) {
		if (paramHandle->module == module && paramHandle->paramId == paramId)
			return paramHandle;
	}
	return NULL;
}

void Engine::updateParamHandle(ParamHandle *paramHandle, int moduleId, int paramId, bool overwrite) {
	VIPLock vipLock(internal->vipMutex);
	std::lock_guard<std::recursive_mutex> lock(internal->mutex);

	// Set IDs
	paramHandle->moduleId = moduleId;
	paramHandle->paramId = paramId;
	paramHandle->module = NULL;

	auto it = std::find(internal->paramHandles.begin(), internal->paramHandles.end(), paramHandle);

	if (it != internal->paramHandles.end() && paramHandle->moduleId >= 0) {
		// Remove existing ParamHandles pointing to the same param
		for (ParamHandle *p : internal->paramHandles) {
			if (p != paramHandle && p->moduleId == moduleId && p->paramId == paramId) {
				if (overwrite)
					p->reset();
				else
					paramHandle->reset();
			}
		}
		// Find module with same moduleId
		for (Module *module : internal->modules) {
			if (module->id == paramHandle->moduleId) {
				paramHandle->module = module;
			}
		}
	}
}


void EngineWorker::run() {
	system::setThreadName("Engine worker");
	system::setThreadRealTime(engine->internal->realTime);
	disableDenormals();

	while (1) {
		engine->internal->engineBarrier.wait();
		if (!running)
			return;
		Engine_stepModules(engine, id);
		engine->internal->workerBarrier.wait();
	}
}


} // namespace engine
} // namespace rack
