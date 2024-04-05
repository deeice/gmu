# Contributing to the Gmu Music Player

Contributions to the Gmu Music Player are welcome. Here are a couple of things
to consider when contributing.

## Bug reports

When reporting bugs, please include as many relevant information as possible. In
case it could be even remotely relevant, please include information about the
hardware/software setup you are experiencing the issue on.

## Contributing code

Here are a few things to keep in mind when preparing a pull request.

1. The Gmu code repository currently has two development branches, the `sdl20`
   branch and the `sdl12` branch. Of those two, the `sdl20` branch is the main
   development branch, while the `sdl12` branch is kept around for the time
   being as the legacy Gmu version, intended for devices that do not support
   SDL2 and require the old SDL 1.2. While I do try to backport all changes that
   are compatible from the `sdl20` branch to the legacy `sdl12` branch, I will
   usually not add any new features to the user interface of the SDL 1.2
   version. If at all possible, it is strongly advised to use the SDL2 version
   of Gmu. If you want to contribute code to the legacy SDL1.2 version, you are
   welcome to do so, although I would suggest you check if it is possible to use
   the newer SDL 2 version on your device instead.
2. Please use short, but descriptive commit messages. A commit message should
   include the component that the commit adds/changes, followed by a colon,
   followed by a short description. You can also add additional context by
   adding a longer description as additional lines. Example commit message:
   `sdl_frontend: Fix data race when resizing window/toggling fullscreen`
3. Keep each commit limited to a single thing. It is okay to touch several files
   in one commit, but those changes should all be related to the one thing that
   you intended to implement.
4. While there is no formal definition of a coding style used in Gmu's codebase,
   I always tried to be consistent. I would suggest you skim through a few
   source files, to get a feel for the coding style used and then stick to that
   in your own code.
5. Having said that, Gmu's codebase is pretty old by now and there are some
   things I would do differently today. When it comes to those things, it is
   fine if you don't follow what you see in Gmu's code. A few things that come
   to mind are:
   - Back when I wrote most of Gmu's code, I liked the idea of having functions
      with a single return. These days I realize that this comes at a cost -
      more complexity, particularily more nesting of code - which could be
      avoided when allowing multiple returns in a function, thus allowing to
      return early from a function. It is therefore perfectly fine and
      encouraged to return early in new code, avoiding unnecessary nesting.
   - Talking about nesting, Gmu's codebase also isn't very consistent when it
      comes to the "happy path". If I were to write the code again, I would try
      to have the "happy path", that is the code that is most likely to run if
      there are no errors or other unusual or rare conditions, to be as flat as
      possible and use nesting mostly for the less common paths. When
      contributing code, I would encourage you to follow this strategy.
   - Gmu also has a few places with very long functions. This is oviously not
      ideal and should be avoided in new code.
   - While there are situations where global variables are helpful and Gmu makes
      use of them in a couple of places, it is generally a good idea to avoid
      them. The same is true for functions with side-effects. If you can avoid
      having side-effects, you should.
6. Other things worth mentioning:
   - When pointer-type function arguments are not supposed to be changed within
      the function (meaning you don't want to return data that way), they should
      be declared as const. Non-const pointer arguments should generally be the
      exception. Only if returning data via a function argument cannot be
      avoided, should this be even considered.

## Licensing

Gmu's code is licensed under the GPLv2 license. By contributing code you agree
that your contributions are placed under this license as well.

## Feature requests

While feature requests are fine, please keep in mind that it might take a while
for me to have a look at them and there is no guarantee I will work on the
requested features. Small and simple feature requests are much more likely to
get implemented than large and complex ones. Contributing code greatly increases
the chance of having the desired feature included in Gmu.
