# Third-party notices

Host glue and runtime changes derive from [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp), pinned in `dependencies.json`. Its MIT license follows.

The combiner shader, GPU vertex shader generation/upload/verification code, and audio page-ownership helper are adapted from [dalves408/nhlhitz2003recomp](https://github.com/dalves408/nhlhitz2003recomp), revision `9e8968bf292059425c0716665b70abf51255043b`. Xbox Debug Kit is fetched separately at its pinned revision and retains its upstream notices.

The MCPX APU derives from xemu; its source files retain their original LGPL notices. The adapted voice page helper is LGPL-2.1-or-later. See [the retained LGPL text](LICENSES/LGPL-2.1.txt) and the complete source changes in `scripts/xboxrecomp-runtime.patch`.

Original game code and assets are not included.

```text
MIT License

Copyright (c) 2026 sp00nz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

```
