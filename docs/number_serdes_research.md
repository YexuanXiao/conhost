# Fast Numeric Serialization/Deserialization Research

This note records internet research used to replace slow `std::stoi`/`wcstoul` style paths with modern fast conversion modules.

## Primary sources reviewed

1. Ryu paper (PLDI 2018, Ulf Adams):
- https://dl.acm.org/doi/10.1145/3192366.3192369
- Core result: fast shortest-decimal float-to-string conversion with correctness guarantees.

2. Ryu reference implementation (author repository):
- https://github.com/ulfjack/ryu

3. Number Parsing at a Gigabyte per Second (Eisel-Lemire):
- https://arxiv.org/abs/2101.11408
- Core result: very fast exact float parsing strategy used broadly in modern parsers.

4. fast_float repository (Eisel-Lemire based implementation):
- https://github.com/fastfloat/fast_float

5. C++ `charconv` (`std::from_chars`/`std::to_chars`) contract:
- https://en.cppreference.com/w/cpp/utility/from_chars
- https://en.cppreference.com/w/cpp/utility/to_chars

## Selected implementation strategy

Because this project forbids third-party dependencies, the replacement uses:
- Independent local module: `new/src/serialization/fast_number.*`
- Standard-library `charconv` APIs for float/integer format/parse on ASCII input.
- Manual wide-character integer parsing and hex parsing to avoid locale-aware, allocation-heavy C-runtime conversions.
- Wide-to-ASCII conversion for float parsing, then `std::from_chars`.

This follows modern high-performance conversion practice while staying within:
- C++23 standard library only,
- no iostream/locale overhead,
- no external libraries,
- no assembly.

## Replacements applied

1. `new/src/cli/console_arguments.cpp`
- Replaced slow numeric parsing (`std::stoi`, `wcstoul`) with `serialization::fast_number` APIs.

2. `new/src/config/app_config.cpp`
- Replaced `std::stoul`-based config integer parsing with fast parser module.

3. Added tests:
- `new/tests/fast_number_tests.cpp`

