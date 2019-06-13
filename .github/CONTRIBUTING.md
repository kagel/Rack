
### Pre-implementation

Before authoring a code contribution, open an issue explaining your proposal in detail.
Remember that code is easy to write, but inventing a valid design is usually the crux.
Expect the proposal to be rejected if you do not consider 100% of use-cases and corner cases.

### Code style and quality

Be courteous and try to match the code style.
I won't define the exact style here, so just follow the existing code.

Use C++11 style, not >=C++14 or C.
This means using the `std::` namespace for stdlib symbols, `std::string` instead of `char*`, `//` comments and `/** */` docstrings, etc.
Avoid over-engineered things like `iostream`, `std::array`, `std::unique_ptr`, etc when simpler alternatives exist.

Write maintainable code that will last >4 years.
No feature/bug is urgent enough to write dirty/temporary implementations.
I have no need to withdraw technical debt.

### Legal

Because a proprietary fork of VCV Rack is planned by VCV (*Rack for DAWs*), in order for your patch to be accepted, you must make a declaration stating that:
- you are the sole author of your patch.
- your patch is released under the [CC0](https://creativecommons.org/publicdomain/zero/1.0/) license.
- or ask for a copyright reassignment form if you prefer this instead.
