# Ikos-llvm#

Ikos-llvm is a static analyzer that computes inductive invariants
using Ikos (a library of abstract domains and fixpoint algorithms
developed at NASA Ames) from LLVM-based languages.

Ikos-llvm provides two standalone tools: `llvmpp` and `llvmikos`:

- `llvmpp`: is a LLVM bytecode preprocessor that applies optimizations
to make easier the task of static analysis.

- `llvmikos`: converts LLVM bitecode into a language-independent CFG
  and computes invariants on it.

The use of `llvmpp` is optional but highly recommended with large
programs.

# Prerequisites #

- The C++ compiler must support c++11

- Boost and gmp

# Installation #

The compilation steps are:

1. ```mkdir build ; cd build```
2. ```cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=my_install_dir ../```

This will install in `my_install_dir/bin` two executables: `llvmikos`
and `llvmpp`.

# Usage #

First, we need to compile a program into `LLVM` bitecode.
 
- `clang -c -emit-llvm file.c -o file.bc` 

The version of `clang` must be compatible with the `LLVM` version used
to build `llvmikos` and `llvmpp`. To see the `LLVM` version, type
`llvmikos --version`.

Note that we can also use `gcc` together with `DragonEgg`.

Then, we can optionally run the preprocessor on the generated
bitecode:

- `llvmpp file.bc -o file.pp.bc` 

The preprocessor can also inline all functions to provide
context-sensitivity:

- `llvmpp file.bc -ikos-inline-all -o file.pp.bc` 

Finally, we can analyze the program by choosing a particular abstract
domain and by considering only live variables:

- `llvmikos file.pp.bc -ikos-domain=ZONES -ikos-live -ikos-answer`

The option `-ikos-answer` displays all the invariants inferred for
each basic block in the `LLVM` bitecode.

#People#

* [Jorge Navas](http://ti.arc.nasa.gov/profile/jorge/)
* [Arie Gurfinkel](arieg.bitbucket.org)
* [Temesghen Kahsai](http://www.lememta.info/)
