---
title: "C++ is not a programming language"
date: 2026-11-18
author: agentty
tags: [c++, compilers, philosophy, essay]
excerpt: "In 1994 a C++ program computed prime numbers without ever compiling. The answers came out in the error messages. Thirty years later, C++26 lets a compiler flag decide whether your safety checks exist. C++ isn't a language for programming computers. It's a configuration format for a compiler, and your app is a side effect."
---

# C++ is not a programming language

### It's a config file for your compiler. Your app is a side effect.

In 1994, at a C++ standards committee meeting in San Diego, Erwin Unruh
handed around a program that computed prime numbers.

It didn't compile.

That was the point. The primes appeared in the **error messages**. The
computation happened entirely inside the compiler, which printed the answers
while refusing to build anything. No binary was produced, nothing ran, and
the work got done anyway.

I think that moment showed what C++ really is, and we've spent thirty years
politely ignoring it:

**C++ is not a language for programming computers. It's a configuration
format for a C++ compiler. You configure the compiler in a certain way, and
an application falls out as a side effect.**

That sounds like a joke. Stay with me, because the longer you look at it,
the less it sounds like one.

## Most of your code never runs

Look at a line of modern C++:

```cpp
template <typename T>
concept Hashable = requires(T a) {
    { std::hash<T>{}(a) } -> std::convertible_to<std::size_t>;
};
```

It's careful, useful, well-engineered code, and it produces **zero bytes** in
your binary. It never executes. No CPU will ever know it existed.

So who is it for?

It's for exactly one reader: the compiler. It tells the compiler which
programs to **refuse**.

The same goes for `const`, `static_assert`, `private`, `[[nodiscard]]`, and
most of the type system. Go through a serious C++ codebase and count how much
of the intelligence in it is aimed at the processor and how much at the
compiler. The processor gets loops and arithmetic. The compiler gets the
cleverness.

agentty is no exception. Its permission matrix is
[proved by the compiler over all 48 cells](/blog/cpp-design-of-a-terminal-agent),
and its lock-ordering rules are types that never reach the binary. A whole
layer of the codebase exists only to make wrong programs fail to build.

If most of what you write is addressed to the build tool rather than to the
machine, you aren't writing a program. You're writing its configuration.

## The compiler runs your program first

Unruh's primes weren't a fluke. In 2003, Todd Veldhuizen showed that C++
templates are Turing-complete: in principle, the compiler can compute
anything before your program exists.

Modern C++ stopped treating this as a party trick and made it a feature.
Here is Unruh's stunt again, in C++20, as perfectly ordinary code:

```cpp
constexpr auto primes = [] {
    std::vector<int> found;
    for (int n = 2; found.size() < 10; ++n)
        if (std::ranges::none_of(found, [n](int p) { return n % p == 0; }))
            found.push_back(n);
    std::array<int, 10> out{};
    std::ranges::copy(found, out.begin());
    return out;
}();

int main() { return primes[4]; }
```

A heap-allocated vector, a loop, a lambda, a search. All of it is run by the
compiler. Here is everything GCC 16 emits for `main`:

```asm
mov  $11, %eax
ret
```

That's the whole program. 11 is the fifth prime. The vector, the loop and
every division were executed once, on a machine you didn't target, at a
moment no user will ever see, and then thrown away.

So which program did you write: the one the compiler ran, or the one it
shipped?

C++ programmers write two programs at once, and only one of them is the one
they think they're writing.

## You don't say what to do. You say what must be true.

C++ has a quiet rule that changes everything, called the **as-if rule**. The
compiler may emit any machine code at all, as long as the program behaves
*as if* it did what you wrote.

So you write this:

```cpp
int sum(int n) {
    int total = 0;
    for (int i = 0; i < n; ++i) total += i;
    return total;
}
```

An optimising compiler is free to delete your loop entirely and replace it
with a closed-form formula, and modern ones do. Your loop, your variable, the
steps you pictured in your head: gone. Not because the compiler ignored you,
but because it took you exactly at your word. You described a **result**, and
it found a cheaper way to get there.

That's the difference between instructions and configuration.

Instructions say *do this*.
Configuration says *this must hold*, and lets the engine decide how.

C++ programmers think they're writing recipes. They're writing constraints.

## The genie problem

In 2009, the Linux kernel shipped code that looked roughly like this:

```c
struct sock *sk = tun->sk;   // use the pointer
if (!tun) return POLLERR;    // ...then check whether it was null
```

Dereferencing a null pointer is undefined behaviour in C (and in C++). So the
compiler reasoned, correctly by the rules, that `tun` couldn't be null, since
it had already been dereferenced, which made the check pointless. It deleted
the check, and the result was an exploitable security hole
([CVE-2009-1897](https://nvd.nist.gov/vuln/detail/CVE-2009-1897)).

Everyone's first reaction is "the compiler broke the kernel." But look at it
as configuration and it's clearer. The source code said *you may assume this
pointer is valid*. The compiler read that literally, the way a parser reads a
YAML file, and acted on it.

Like a genie, the compiler grants exactly what you asked for, not what you
wanted.

The best detail is how part of the fix went in. The kernel added a compiler
**flag**, `-fno-delete-null-pointer-checks`, so that a security bug in a
program was addressed in part by changing a setting.

If you can fix your program by editing configuration, it's worth asking which
of the two was the program all along.

## Same source, different apps

Take one `.cpp` file. Build it with `-O0`. Then `-O3`. Then `-ffast-math`.
Then `-march=native`.

You get different binaries: different speed, different floating-point
results, and, if your code relies on undefined behaviour anywhere, possibly
completely different behaviour.

So which one is "the program"?

The source file isn't the app. It's one input. The flags are another, the
compiler version a third, the target CPU a fourth. The app is what comes out
when all of them are combined.

Then look at what surrounds it: a CMake file that configures Ninja, which
configures the compiler, which reads your C++, which configures the compiler
some more. We know this one personally. agentty's build reads how much RAM
your machine has and sizes Ninja's job pools so that configuring the
configurator doesn't swap your laptop to death.

It's configuration all the way down, and somewhere at the bottom, almost by
accident, an executable appears.

## Modern C++ stopped pretending

If this still sounds like a metaphor, look at what the last three standards
actually added. Nearly every major feature since C++20 is a new way to talk
to the compiler, not the CPU. Every example below was compiled for this post
with GCC 16.2, and every output shown is real.

**C++20: the compiler reads your strings.**

```cpp
std::format("{} items cost {:.2f}", n, price);  // fine
std::format("{:d}", "hello");                    // build fails
```

A bad format string used to be a crash in production. Now the compiler
parses the string, checks every placeholder against its argument, and refuses
to build. A string literal became part of the type system.

**C++23: you hand the optimiser an axiom.**

```cpp
int div4(int x) {
    [[assume(x >= 0)]];
    return x / 4;
}
```

Without the attribute, GCC emits four instructions, because dividing a
negative number needs a fix-up. With it, two: `mov` and `sar`. The attribute
is never checked and never runs. It's a sentence you write *to the compiler*,
and if it's false, the behaviour is undefined. You aren't describing what
the program does any more. You're adding a premise to the proof the compiler
is building about it.

**C++26: you write the judge's rulings.**

```cpp
void take(const int*) = delete("pass a std::span, not a raw pointer");
```

Call it and the build fails with exactly your sentence:
`error: use of deleted function 'void take(const int*)': pass a std::span,
not a raw pointer`. `static_assert` can now print a computed message too. The
error message, the part of C++ everyone mocks, has become something you
design.

**C++26: whether the check exists is a build setting.** This is the one that
turns the joke into a specification.

```cpp
double checked_sqrt(double x)
    pre(x >= 0)
    post(r: r >= 0);
```

That's a contract: a precondition and a postcondition, in the language at
last. But the standard doesn't say whether the check runs. It defines four
evaluation semantics, *ignore*, *observe*, *enforce* and *quick_enforce*, and
leaves the choice to the build. I compiled this one file four times and
called it with `-1`:

| `-fcontract-evaluation-semantic=` | What the program did |
|---|---|
| `ignore` | returned, said nothing |
| `observe` | printed the violation, kept going |
| `enforce` | printed the violation, aborted |
| `quick_enforce` | aborted on the spot, without saying which check failed |

Same source, same input, four different programs. Whether your safety check
exists is not written in your code. It's a setting.

**C++26: the compiler reads your program, and writes more of it.**

```cpp
template <typename T>
void dump(const T& obj) {
    constexpr auto ctx = std::meta::access_context::unchecked();
    template for (constexpr auto m :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx)))
        std::println("{} = {}", std::meta::identifier_of(m), obj.[:m:]);
}
```

`dump(User{"ada", 36, true})` prints `name = ada`, `age = 36`,
`admin = true`. Nobody wrote code that knows `User`'s fields, and there's no
loop at runtime. During the build, the compiler took your struct as a value
(`^^T`), walked its members, and pasted one print statement per field back
into your function (`[:m:]`). The configuration now reads itself.

**C++26: the compiler reads your files.**

```cpp
constexpr unsigned char logo[] = {
#embed "logo.png"
};
```

The image is in your binary because the compiler opened it.

Line those up and the direction is impossible to miss. C++11 let the
compiler evaluate a function. C++20 let it allocate memory and parse your
strings. C++23 let you hand it axioms. C++26 lets it decide which checks
exist, read your types, write your code and open your files. Every new
standard moves more of what a C++ program *is* out of the run and into the
build. The CPU's share of the language keeps shrinking, and the compiler's
keeps growing.

## "But then every language is configuration"

Yes. That's the uncomfortable part.

Python is configuration for CPython. Java is configuration for the JVM.
JavaScript is configuration for V8. Nobody has written instructions a CPU
executes directly since we stopped toggling switches on front panels. Every
program is input to some other program that does the real work.

But most languages hide the middleman. Python doesn't want you thinking about
the interpreter, and Java doesn't want you thinking about the JIT. The
abstraction is the product.

C++ is different. **C++ hands you the middleman's keys.** Templates,
`constexpr`, concepts, attributes and flags program the compiler itself,
while it runs, before your program exists. You make it compute. You make it
refuse. You make it prove things.

Other languages are configuration you're not supposed to notice. C++ is
configuration that admits it.

## So what is a C++ programmer?

Not someone who gives a computer instructions.

A C++ programmer writes a **contract** for a reader that is extraordinarily
powerful, completely literal and indifferent to what you meant.

The standard is the law, the compiler is the judge, and the binary is the
ruling.

Seen that way, the strangest parts of C++ stop looking strange:

- **The error messages run to three hundred lines** because the judge is
  citing precedent.
- **Templates feel like dark magic** because you're programming the judge.
- **Undefined behaviour is terrifying** because you signed something you
  didn't read.
- **The best C++ engineers aren't the fastest coders.** They're the best
  lawyers.

## The punchline

We like to believe we tell machines what to do.

But the closer you get to the metal, the more programming looks like
persuasion. You describe a world: these types, these guarantees, these things
that can never happen. Then a machine that is far smarter than you, in one
narrow way, finds the cheapest program consistent with your description.

Your app isn't what you wrote. It's what the compiler concluded.

Every language works this way. C++ is just the one that makes you feel it.
