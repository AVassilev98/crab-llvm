#Crab-llvm#

<img src="https://upload.wikimedia.org/wikipedia/en/4/4c/LLVM_Logo.svg" alt="llvm logo" width=280 height=200 /> 
<img src="http://i.imgur.com/IDKhq5h.png" alt="crab logo" width=280 height=200 /> 

#About#

Crab-llvm is a static analyzer that computes inductive invariants
based on [Crab](https://github.com/seahorn/crab) for LLVM-based
languages.

Crab-llvm provides two standalone tools: `crabllvmpp` and `crabllvm`:

- `crabllvmpp`: is a LLVM bytecode preprocessor that applies optimizations
to make easier the task of static analysis.

- `crabllvm`: converts LLVM bitcode into a language-independent CFG
  and computes invariants from it.

The use of `crabllvmpp` is optional but highly recommended with large
programs.

#License#

Crab-llvm is distributed under MIT license. See
[LICENSE.txt](LICENSE.txt) for details.

#Installation#

Crab-llvm is written in C++ and uses heavily the Boost library. You will need:

- C++ compiler supporting c++11
- Boost and Gmp

If you want Crab-llvm to reason about pointers and arrays you need to
download the following package at the root directory:

* [dsa-seahorn](https://github.com/seahorn/dsa-seahorn): ``` git clone https://github.com/seahorn/dsa-seahorn.git ```

Another optional component used by `crabllvmpp` is:

* [llvm-seahorn](https://github.com/seahorn/llvm-seahorn): ``` git clone https://github.com/seahorn/llvm-seahorn.git```

`llvm-seahorn` provides specialized versions of `InstCombine` and
`IndVarSimplify` LLVM passes as well as a LLVM pass to convert
undefined values into nondeterministic calls.

Then, the compilation steps are:

1. ```mkdir build ; cd build```
2. ```cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=my_install_dir ../```

If you want to use the boxes domain then add to step 2 the option `-DUSE_LDD=ON`.


#Usage#

Crab-llvm provides a python script called `crabllvm.py` to interact
with users. Given a C program, users just need to type: `crabllvm.py
file.c --crab-print-invariants`.

- The option `--crab-print-invariants` displays all the invariants
inferred for each basic block in the `LLVM` bitcode.

- Users can also choose the abstract domain by typing the option
`--crab-domain`. The possible values are:

    - `int`: classical intervals
	- `ric`: intervals with congruences
	- `zones`: difference-bound matrices
	- `num`: select dynamically between `int` and `zones`
    - `term`: intervals with uninterpreted functions
    - `boxes`: disjunctive intervals (only if `-DUSE_LDD=ON`)
	
	   For boxes, we may want also to set the options:
	   
       - `--crab-narrowing-iterations=N`
	   - `--crab-widening-threshold=W`

       where `N` is the number of descending iterations and `W` is the
       number of fixpoint iterations before triggering widening. In
       particular, a small value for `N` might be needed (e.g.,
       `n=2`). For `W` it really depends on the program.

- We also provide the option `--crab-track` to indicate the level
of precision. The possible values are: 

    - `int`: reasons about integer scalars (LLVM registers).
	- `ptr`: reasons about pointer addresses.	
    - `arr`: reasons about the contents of pointers and arrays.

   If the level is `ptr` then Crab-llvm reasons about pointer
   arithmetic but it does not translate neither LLVM loads nor stores.
   
   If the level is `arr` then Crab-llvm uses the heap analysis
   provided by `dsa-seahorn` to partition the heap into disjoint
   arrays (i.e., sequence of consecutive bytes). Each LLVM load and
   store is translated to an array read and write operation,
   respectively. Then, it will use an array domain provided by Crab
   whose base domain is the one selected by option
   `--crab-domain`. Unlike `int` and `ptr` which produce a concrete
   semantics (up to the selected level of precision), the level `arr`
   produces an abstract semantics where all memory contents are
   already over-approximated. Nevertheless, Crab's analyses can always
   refine this abstract semantics by using more precise pointer and/or
   value analyses.

   Regardless the level of precision, Crab-llvm can try to resolve
   indirect calls if `dsa-seahorn` is installed and enable option
   `--crab-devirt`.

- By default, all the analyses are run in an intra-procedural
  manner. Enable the option `--crab-inter` to run the inter-procedural
  version. Crab-llvm implements a standard two-phase algorithm in
  which the call graph is first traversed from the leaves to the root
  while computing summaries and then from the root the leaves reusing
  summaries. Each function is executed only once. The analysis is
  sound with recursive functions but very imprecise. The option
  `--crab-print-summaries` displays the summaries for each
  function. The inter-procedural analysis is specially important if
  reasoning about memory contents is desired.

- To make easier the communication with other LLVM-based tools,
  Crab-llvm can output the invariants by inserting them into the LLVM
  bitecode via `verifier.assume` instructions. The option
  `--crab-add-invariants-at-entries` injects the invariants that hold
  at each basic block entry while option
  `--crab-add-invariants-after-loads` injects the invariants that hold
  right after each LLVM load instruction. To see the final LLVM
  bitecode just add the option `-o out.bc`.
  
Take the following program:

```c

    extern int nd ();
    int a[10];
    int main (){
       int i;
       for (i=0;i<10;i++) {
         if (nd ())
            a[i]=0;
         else 
            a[i]=5;
	   }		 
       int res = a[i-1];
       return res;
    }
```

and type `crabllvm.py test.c --crab-live --crab-track=arr --crab-add-invariants-at-entries --crab-add-invariants-after-loads -o test.crab.bc`. The content of `test.crab.bc` should be similar to this:


```

    define i32 @main() #0 {
    entry:
       br label %loop.header
    loop.header:   ; preds = %loop.body, %entry
       %i.0 = phi i32 [ 0, %entry ], [ %_br2, %loop.body ]
       %crab_2 = icmp ult i32 %i.0, 11
       call void @verifier.assume(i1 %crab_2) #2
       %_br1 = icmp slt i32 %i.0, 10
       br i1 %_br1, label %loop.body, label %loop.exit
    loop.body:   ; preds = %loop.header
       call void @verifier.assume(i1 %_br1) #2
       %crab_14 = icmp ult i32 %i.0, 10
       call void @verifier.assume(i1 %crab_14) #2
       %_5 = call i32 (...)* @nd() #2
       %_6 = icmp eq i32 %_5, 0
       %_7 = sext i32 %i.0 to i64
       %_. = getelementptr inbounds [10 x i32]* @a, i64 0, i64 %_7
       %. = select i1 %_6, i32 5, i32 0
       store i32 %., i32* %_., align 4
       %_br2 = add nsw i32 %i.0, 1
       br label %loop.header
    loop.exit:   ; preds = %loop.header
       %_11 = add nsw i32 %i.0, -1
       %_12 = sext i32 %_11 to i64
       %_13 = getelementptr inbounds [10 x i32]* @a, i64 0, i64 %_12
       %_ret = load i32* %_13, align 4
       %crab_23 = icmp ult i32 %_ret, 6
       call void @verifier.assume(i1 %crab_23) #2
       ret i32 %_ret
    }
```

The special thing about the above LLVM bitecode is the existence of
`@verifier.assume` instructions. For instance, the instruction
`@verifier.assume(i1 %crab_2)` indicates that `%i.0` is between 0 and
10 at the loop header. Also, `@verifier.assume(i1 %crab_23)` indicates
that the result of the load instruction at block `loop.exit` is
between 0 and 5.


#Known Limitations#

- Variadic functions are ignored.
- Floating point operations are ignored.
- ...

#People#

* [Jorge Navas](http://ti.arc.nasa.gov/profile/jorge/)
* [Arie Gurfinkel](arieg.bitbucket.org)
* [Temesghen Kahsai](http://www.lememta.info/)
