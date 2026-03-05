
# Standards and Guidelines

## Develop principles

* One firmware per board containing everything, to keep management of firmware simple - for everybody. 3MB flash space to allow for this.
* Part of submitting a change via a pull request is updated documentation. Functionality and documentation should be in one Pull Request 
* Make minimal changes in upstream (Sveltekit) code, as we need to stay in sync as easy as possible. Add // 🌙 to show a change has been made.
* The main branch is the source to branch and merge to, no direct code commits to the main branch. As the main branch docs folder is the source for the website, doc changes can be made directly to main. 
* Branches from main are made for latest updates and merged back to main when done.
* The src folder is for all MoonBase and MoonLight Nodes and Modules development. No need for UI changes as that is generated for Nodes and Modules. The lib folder is for upstream (Sveltekit). The interface folder is for UI, mainly Sveltekit and Modules and Nodes generic functions.
* A pull request should contain compilable code that is tested to, at minimum, not crash the system and also supports boards without PSRAM, e.g. ESP32-D0. Code may be work in progress.

## Pull Requests (PR)

### Create a PR

Make all PRs against the `main` branch: Create a branch from the main branch first.

Please add a description of your proposed code changes. It does not need to be an exhaustive essay, however a PR with no description or just a few words might not get accepted, simply because very basic information is missing.

A good description helps us to review and understand your proposed changes. For example, you could say a few words about

* what you try to achieve (new feature, fixing a bug, refactoring, security enhancements, etc.)
* how your code works (short technical summary - focus on important aspects that might not be obvious when reading the code)
* testing you performed, known limitations, open ends you possibly could not solve.
* any areas where you like to get help from an experienced maintainer

### Updating your code

While the PR is open - and under review by maintainers - you may be asked to modify your PR source code.
You can simply update your own branch, and push changes in response to reviewer recommendations. 
Github will pick up the changes so your PR stays up-to-date.

!!! warning "Do not use force-push while your PR is open!"
    * It has many subtle and unexpected consequences on our GitHub repository.
    * For example, we regularly lost review comments when the PR author force-pushes code changes. So, pretty please, do not force-push.

You can find a collection of very useful tips and tricks here: [How to properly submit a PR](https://github.com/wled-dev/WLED/wiki/How-to-properly-submit-a-PR)

The 🐰 (see AI) will review each commit, please process the review recommendations.

### Merge a PR

Before merging a PR back into main ask the 🐰 the following:

@coderabbitai, I am about to merge this PR, please do an in-depth review of all the commits made, make a summary,  a recommendation to merge and a list of possible future actions.

## Artificial Intelligence

MoonLight supports the use of AI. Using AI is a hot topic and different people have different opinions. The reason MoonLight supports it is because to talk about it you need to know about it. So we will investigate the use of it and monitor learnings and change use of it accordingly. Below the principles we are using, of which Reversible is the most important: MoonLight should never be depending on AI !!!

### AI Principles

MoonLight currently uses CodeRabbit and Claude Code AI tools, using the following principles: **4EP, Unit tests and Reversible**:

**4 Eyes Principle (4EP)**

For AI-generated code, the 4 Eyes Principle (**4EP**) applies: 

  * Code is added via commits to (feature) branches on the main branch and via Pull Requests merged back into main
  * Code generated with Claude Code must be reviewed by the developer 👀 before committing it to a feature branch
  * Coderabbit automatically reviews each commit 👀 in a Pull Request
  * Coderabbit reviews must be processed by the developer and resolved (following above steps)

**Unit tests**

Unit tests need to be added for AI and non-AI code (Claude code can assist in creating unit tests). The following unit test tools are used:

* backend: 
     * cppcheck: see [pio.ini](https://github.com/MoonModules/MoonLight/blob/main/platformio.ini) for compile checks (less strict) and [lint.yml](https://github.com/MoonModules/MoonLight/blob/main/.github/workflows/lint.yml) for CI checks (more strict). Ignores are added in the code by using // cppcheck-suppress
     * doctest: see https://github.com/MoonModules/MoonLight/tree/main/test/test_native, run by [lint.yml](https://github.com/MoonModules/MoonLight/blob/main/.github/workflows/lint.yml) by compiling env:native
* frontend: 
    * Vitest see [moonbase_utilities.test](https://github.com/MoonModules/MoonLight/blob/main/interface/src/lib/stores/moonbase_utilities.test.ts)
    * ESLint + Prettier, See [lint.yml](https://github.com/MoonModules/MoonLight/blob/main/.github/workflows/lint.yml)
* experimental: IWYU, See [lint.yml](https://github.com/MoonModules/MoonLight/blob/main/.github/workflows/lint.yml)
* Unit tests are part of the CI, See [lint.yml](https://github.com/MoonModules/MoonLight/blob/main/.github/workflows/lint.yml)

**Reversible**

AI tools need to be **reversible** at all times

* Development should never be depending on any AI tool. A commit must always be possible without using AI tools
* It should be possible at any moment to stop using a specific tool or add / replace another tool.

### Contributing

It's OK if you took help from an AI for writing your source code. 
    
However, we expect a few things from you as the person making a contribution to MoonLight:

 * Make sure you really understand the code suggested by the AI, and don't just accept it because it "seems to work".
 * Don't let the AI change existing code without double-checking by you as the contributor. Often, the result will not be complete. For example, previous source code comments may be lost.
 * If you don't feel very confident using English, you can use AI for translating code comments and descriptions into English. AI bots are very good at understanding language. However, always check if the results is correct. The translation might still have wrong technical terms, or errors in some details.

### Best practices

* As the person who contributes source code to MoonLight, make sure you understand exactly what the AI-generated code does
* Add a comment like ``'// below section of my code was generated by an AI``, when larger parts of your source code were not written by you personally.
* Always review translations and code comments for correctness
* Always review AI-generated source code
* If the AI has rewritten existing code, check that the change is necessary and that nothing has been lost or broken. Also check that previous code comments are still intact.

## Code style

### Code

We use Clang-format for c-files (see .clang-format in the repo for the definition) and Prettier for front-end files.

Right-click Format Document on each file you edit before committing.

### Comments

Comments are used for technical documentation, always document your changes, each function must have a comment describing what it does, including its arguments. Comments are also formatted using Clang-format and Prettier.
(Functional documentation must be done on the MoonLight website, see above)

Examples:  
```markdown
// This is a comment.

/* This is a CSS inline comment */

/* 
 * This is a comment
 * wrapping over multiple lines,
 * used in MoonLight for file headers and function explanations
 */

<!-- This is an HTML comment -->
```

There is no set character limit for a comment within a line, though as a rule of thumb you should wrap your comment if it exceeds the width of your editor window.  

Inline comments are OK if they describe that line only and are not exceedingly wide.

(modified version from WLED-MM, 🙏 to @softhack007)