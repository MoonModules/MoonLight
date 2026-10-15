"""check_docgen.py guards the generated documentation, so this guards it back.

The rules it enforces (documentation-standards.md § The card) are invisible when they
break: a regex that stops matching makes the check print a clean run, which is
indistinguishable from a tree with nothing wrong in it. Four `## HLS, details` headings
lived in the tree unlinked for exactly that reason, found by adding the rule rather than
by the rule working.

So each rule is pinned twice: it FIRES on a page written to break it, and it stays SILENT
on a page that obeys. A test that only asserted the silence would pass against a check
that had stopped reading anything at all.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

from check_docgen import _header_rules  # noqa: E402


def test_a_wrapped_sentence_in_a_class_comment_is_a_finding():
    # The one-line budget cannot reach here: a class comment is ALLOWED ten lines, so a
    # sentence carried onto the next line passes every other rule.
    header = ("#pragma once\nnamespace mm {\n"
              "/// A sentence that runs long and wraps onto\n"
              "/// a second line, which is what this catches.\n"
              "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    wraps = [w for _, w in _header_rules("probe.h", header) if "hard wrap" in w]
    assert len(wraps) == 1


def test_two_sentences_that_fit_one_line_are_a_split_paragraph():
    # Each line ends a sentence, so neither is wrapped, but markdown joins them into one paragraph and the split only spends a line.
    split = ("#pragma once\nnamespace mm {\n"
             "/// Boot applies it as a stored file.\n"
             "/// A module main.cpp wires stays.\n"
             "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    assert len([w for _, w in _header_rules("probe.h", split) if "split paragraph" in w]) == 1


def test_a_paragraph_too_long_for_one_line_is_cut_not_split():
    # One topic is one paragraph and one line: sentence lines read as separate topics, so past the cap the paragraph is cut, never split.
    long = "word " * 40
    header = ("#pragma once\nnamespace mm {\n"
              f"/// {long.strip()} one.\n"
              f"/// {long.strip()} two.\n"
              "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    splits = [w for _, w in _header_rules("probe.h", header) if "split paragraph" in w]
    assert len(splits) == 1
    assert "cut it to fit one line" in splits[0]


def test_a_paragraph_over_several_lines_is_one_finding_at_its_start():
    # Joining pair by pair packed a long paragraph into line-sized chunks; one finding per paragraph names the whole of it.
    header = ("#pragma once\nnamespace mm {\n"
              "/// First sentence.\n"
              "/// Second sentence.\n"
              "/// Third sentence.\n"
              "/// Fourth sentence.\n"
              "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    splits = [w for _, w in _header_rules("probe.h", header) if "split paragraph" in w]
    assert len(splits) == 1
    assert "lines 3-6" in splits[0] and "join it into one line" in splits[0]


def test_a_continuation_that_does_not_start_lowercase_is_still_a_wrap():
    # The test is the PREVIOUS line's unfinished sentence, not the next line's case. A
    # continuation beginning with a digit or an identifier is most of a technical comment,
    # and a lowercase-only test missed every one of them.
    digit = ("#pragma once\nnamespace mm {\n"
             "/// A sentence that does not end here and gives\n"
             "/// 8-16 lanes for the wall time of one.\n"
             "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    ident = ("#pragma once\nnamespace mm {\n"
             "/// The wire format is\n"
             "/// ParallelSlots.h in the same directory.\n"
             "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    for header in (digit, ident):
        assert len([w for _, w in _header_rules("probe.h", header) if "hard wrap" in w]) == 1


def test_an_indented_block_keeps_its_layout():
    # A wire table, a listing or a diagram is a BLOCK: joining its lines destroys the layout
    # that carries the meaning. Collapsing one produced a 43-word "sentence" of protocol bytes
    # in PreviewDriver.h, which is the worst of both readings.
    table = ("#pragma once\nnamespace mm {\n"
             "/// Lead.\n///\n/// @technical\n///\n/// ## S\n///\n"
             "///     POST /api            pair, once pressed\n"
             "///     GET  /api/lights     the lights by id\n"
             "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    assert not [w for _, w in _header_rules("probe.h", table) if "hard wrap" in w]


def test_a_colon_or_semicolon_does_not_end_a_sentence():
    # A colon INTRODUCES what follows and a semicolon joins two clauses, so neither ends a
    # thought. Unicode's sentence-boundary rules, Vale's own detector and the standards line
    # agree ("joined by a comma or a colon that a full stop should have been"). Treating them
    # as terminators passed every `Prior art: ...` block that wrapped mid-sentence.
    colon = ("#pragma once\nnamespace mm {\n"
             "/// Prior art: one discovery, one lineage, one driver:\n"
             "/// architecture studied, never copied.\n"
             "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    semi = ("#pragma once\nnamespace mm {\n"
            "/// The difference is underneath;\n"
            "/// it buys two things the other cannot give.\n"
            "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    for header in (colon, semi):
        assert len([w for _, w in _header_rules("probe.h", header) if "hard wrap" in w]) == 1


def test_structure_is_not_read_as_a_wrapped_sentence():
    # A list item, a heading and a table row continue across lines as STRUCTURE. Reading
    # one as wrapped prose would fire on every appendix that carries a table.
    header = ("#pragma once\nnamespace mm {\n"
              "/// A finished sentence.\n"
              "///\n"
              "/// @technical\n"
              "///\n"
              "/// ## A section\n"
              "///\n"
              "/// - an item that continues\n"
              "///   onto the next line\n"
              "///\n"
              "/// | a | b |\n"
              "/// |---|---|\n"
              "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    assert not [w for _, w in _header_rules("probe.h", header) if "hard wrap" in w]


# ---- the header `///` budget ----

def _hdr(text: str):
    import check_docgen
    return check_docgen._header_rules("h.h", text)


def test_a_class_comment_past_ten_lines_is_flagged():
    """The class `///` is the module's summary, not its manual: cut it, and only a fact the code does not say goes after `@technical`."""
    doc = "\n".join(f"/// line {i}" for i in range(12))
    issues = _hdr(doc + "\nclass Foo : public Bar {")
    assert issues and "class comment 12 lines" in issues[0][1]


def test_a_ten_line_class_comment_is_accepted():
    doc = "\n".join(f"/// line {i}" for i in range(10))
    assert not [i for i in _hdr(doc + "\nclass Foo {") if "class comment" in i[1]]


def test_a_member_comment_past_one_line_is_flagged():
    """One line beside the thing it describes. The generated page shows the first
    sentence as the summary, so a second line is the author still talking."""
    issues = _hdr("#include <x.h>\n\nvoid g();\n\n/// one\n/// two\nvoid doThing();")
    assert issues and "member comment 2 lines" in issues[0][1]


def test_a_one_line_member_comment_is_accepted():
    assert not [i for i in _hdr("#include <x.h>\n\nvoid g();\n\n/// one\nvoid doThing();") if "member comment" in i[1]]


def test_a_header_of_free_functions_leads_with_its_own_block():
    """A header of free functions opens with a `///` block saying what the file is for."""
    src = "#pragma once\n\n/// What it is.\n\nvoid doThing();"
    assert not [i for i in _hdr(src) if "file" in i[1]]


def test_a_single_class_header_leads_with_its_class_comment():
    """The class comment IS the file's documentation where the header holds one class."""
    src = "#pragma once\nnamespace mm {\n/// What it is.\nclass Foo {};\n}"
    assert not [i for i in _hdr(src) if "file" in i[1]]


def test_a_header_opening_with_a_slash_slash_block_is_flagged():
    """Doxygen reads `//` as a note to the next reader of the source, so it generates nothing.

    Four platform headers opened this way and their pages were a bare member list."""
    src = "#pragma once\n\n// What it is.\nvoid doThing();"
    issues = [i for i in _hdr(src) if "file opens with //" in i[1]]
    assert issues


def test_a_header_with_no_lead_comment_is_flagged():
    src = "#pragma once\n#include <cstdint>\nvoid doThing();"
    assert [i for i in _hdr(src) if "file lead missing" in i[1]]


def test_an_xref_to_a_real_heading_resolves():
    """`@xref{anchor}` names a heading on the same generated page, by its MkDocs slug."""
    src = ("#pragma once\n/// Lead.\n/// See @xref{terminology|Terminology}.\n///\n"
           "/// @technical\n///\n/// ## Terminology\n///\n/// Words.\nclass Foo {};")
    assert not [i for i in _hdr(src) if "@xref" in i[1]]


def test_an_xref_naming_no_heading_is_flagged():
    """A renamed heading leaves the link dead, and the generator strips it without a word.

    That is the whole reason to check it here: the cross-reference vanishes from the page and
    nothing else reports the loss."""
    src = ("#pragma once\n/// Lead.\n/// See @xref{glossary|Glossary}.\n///\n"
           "/// @technical\n///\n/// ## Terminology\n///\n/// Words.\nclass Foo {};")
    assert [i for i in _hdr(src) if "names no heading" in i[1]]


def test_a_technical_section_past_ten_lines_is_flagged():
    """The cap is PER SECTION, not per appendix.

    A whole-appendix budget punishes a file for having several distinct topics, and the cheapest
    way to satisfy it is to delete a section rather than tighten the prose. Each `## ` section is
    asked to be disciplined instead, so a file may carry as many as it genuinely has."""
    body = "".join(f"/// line {i}\n" for i in range(11))
    src = ("#pragma once\n/// Lead.\n///\n/// @technical\n///\n"
           "/// ## Topic\n///\n" + body + "class Foo {};")
    assert [i for i in _hdr(src) if "appendix section 11 lines" in i[1]]


def test_a_fenced_block_does_not_count_toward_the_section_cap():
    """A fenced block is structure rather than prose, the exemption the no-wrap rule already makes.

    A protocol listing is as long as the thing it describes, so counting its lines would ask the
    author to delete wire format to fit a prose budget."""
    fence = "///\n".join(f"/// [0x{i:02x}][count][stride]\n" for i in range(12))
    src = ("#pragma once\n/// Lead.\n///\n/// @technical\n///\n"
           "/// ## Wire format\n///\n/// ```text\n" + fence + "/// ```\nclass Foo {};")
    assert not [i for i in _hdr(src) if "appendix section" in i[1]]


def test_two_short_technical_sections_are_accepted():
    """Sixteen lines across two sections is fine; one section of eleven is not."""
    a = "".join(f"/// a{i}\n" for i in range(8))
    b = "".join(f"/// b{i}\n" for i in range(8))
    src = ("#pragma once\n/// Lead.\n///\n/// @technical\n///\n"
           "/// ## A\n///\n" + a + "///\n/// ## B\n///\n" + b + "class Foo {};")
    assert not [i for i in _hdr(src) if "appendix section" in i[1]]


def test_a_technical_on_a_member_is_flagged():
    """`@technical` is an appendix, and only a file or class lead carries one.

    Relaxing this put a four-line block on every function in a swept header, which cost 230 lines
    to undo. A member gets one line, because the page shows that line as its summary."""
    src = ("#pragma once\n/// Lead.\n\n"
           "/// Doc.\n///\n/// @technical detail\nvoid doThing();")
    assert [i for i in _hdr(src) if "@technical on a member" in i[1]]


def test_a_technical_on_a_struct_is_accepted():
    """A struct comment is a lead, so it carries an appendix like a class does."""
    src = "#pragma once\n/// Doc.\n///\n/// @technical detail\nstruct S { int a; };"
    assert not [i for i in _hdr(src) if "@technical on a member" in i[1]]


def test_a_technical_does_not_exempt_a_member_comment():
    """`@technical` is the CLASS comment's appendix. A member gets one line, whatever it holds.

    Relaxing this let a four-line block stand on every function in a swept header. The budget is
    one line because the generated page shows that line as the summary; depth belongs on the
    class comment or the module's page."""
    src = ("#include <x.h>\n\nvoid g();\n\n"
               "/// one\n///\n/// @technical a detail\n/// and another\nvoid doThing();")
    issues = [i for i in _hdr(src) if "member comment" in i[1]]
    # TWO, not four: the blank `///` and the `@technical` marker are structure, and the cap counts
    # the prose. What the test pins is that `@technical` buys a member no exemption at all.
    assert issues and "member comment 2 lines" in issues[0][1]


def test_a_code_comment_run_past_one_line_is_flagged():
    """`//` carries the same one-line budget as `///`. Without that the `///` cap moves text
    rather than removing it: a fifty-line member comment re-spelled as `//` passes every other
    rule and the file is exactly as long, which is what a first sweep of these headers did."""
    run = "\n".join(f"// line {i}" for i in range(2))
    issues = _hdr("class Foo {\npublic:\n" + run + "\n/// does a thing\nvoid doThing();\n};")
    assert any("code comment 2 lines" in why for _, why in issues)


def test_a_long_code_comment_line_is_flagged():
    """A `//` line carries the same word budget as a `///` one: without it the line cap is
    satisfied by one very long line, which is a paragraph that happens to lack line breaks."""
    long = "// " + " ".join(["word"] * 35)
    issues = _hdr("class Foo {\npublic:\n" + long + "\n/// does a thing\nvoid doThing();\n};")
    assert any("comment line 35 words" in why for _, why in issues)


def test_a_one_line_code_comment_is_accepted():
    """One line is room to say WHY. The rule cuts essays, not reasons."""
    issues = _hdr("class Foo {\npublic:\n// why\n/// does a thing\nvoid doThing();\n};")
    assert not [i for i in issues if "code comment" in i[1]]


def test_a_file_level_code_comment_is_exempt():
    """The `//` block above the first class explains the compilation unit: it is the
    non-Doxygen sibling of the class comment and has no member to sit beside."""
    run = "\n".join(f"// line {i}" for i in range(12))
    issues = _hdr(run + "\nclass Foo {\npublic:\n/// does a thing\nvoid doThing();\n};")
    assert not [i for i in issues if "code comment" in i[1]]


def test_a_long_doc_sentence_is_flagged():
    long = "/// " + " ".join(["word"] * 35)
    issues = _hdr(long + "\nvoid doThing();")
    assert any("doc sentence 35 words" in why for _, why in issues)


def test_several_short_sentences_on_one_line_are_not_flagged():
    # The no-wrap rule makes a line a paragraph, so the cap counts SENTENCES. Counting the
    # line would fail correct prose and pass nothing the standards ask for.
    line = "/// " + " ".join("A short thought here." for _ in range(4))
    issues = _hdr(line + "\nvoid doThing();")
    assert not [i for i in issues if "doc sentence" in i[1]]


def test_an_undivided_technical_appendix_is_flagged():
    """An appendix with no `## ` heading is one section, so the per-section cap catches it whole.

    This was a 20-line whole-appendix cap. Counting per section instead lets a file carry as many
    topics as it genuinely has, while still refusing a single undivided wall of prose."""
    body = "\n".join(f"/// deep {i}" for i in range(25))
    issues = _hdr("/// @technical\n" + body + "\nclass Foo {")
    assert any("appendix section 25 lines" in why for _, why in issues)


def test_an_undocumented_public_member_is_flagged():
    """A member with no `///` renders as a bare signature, which tells a reader nothing
    the declaration did not."""
    src = "class Foo {\npublic:\n  uint8_t count = 0;\n  void doThing();\n};"
    issues = _hdr(src)
    whys = [why for _, why in issues]
    assert "public variable has no ///" in whys
    assert "public function has no ///" in whys


def test_a_function_carrying_an_attribute_macro_is_still_seen():
    """A capitals macro after the parameter list, such as `MM_NONBLOCKING`, hid 25 public functions from the rule."""
    for decl in ("void tick() MM_NONBLOCKING override {}", "uint16_t count() const MM_NONBLOCKING;",
                 "void tick1s() MM_NONBLOCKING final;", "virtual void set(int v) = 0;"):
        whys = [why for _, why in _hdr(f"class Foo {{\npublic:\n  {decl}\n}};")]
        assert "public function has no ///" in whys, decl


def test_a_documented_public_member_is_accepted():
    src = ("class Foo {\npublic:\n  /// how many\n  uint8_t count = 0;\n"
           "  /// does the thing\n  void doThing();\n};")
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_private_member_needs_no_doc():
    """The budget is about the GENERATED page, and a private member never reaches it."""
    src = "class Foo {\nprivate:\n  uint8_t hidden_ = 0;\n};"
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_statement_inside_a_function_body_is_not_a_member():
    """`return true;` and a local variable parse as declarations, and an inline body keeps
    `public:` in scope, so 251 statements were reported as undocumented public members
    before the brace depth was tracked."""
    src = ("class Foo {\npublic:\n"
           "  /// does the thing\n"
           "  bool doThing() {\n"
           "    Preset& p = presets_[count_];\n"
           "    return true;\n"
           "  }\n};")
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_real_member_beside_an_inline_body_is_still_seen():
    """The guard must not silence the rule: a genuine undocumented member declared after
    a function with an inline body is still reported."""
    src = ("class Foo {\npublic:\n"
           "  /// does the thing\n"
           "  bool doThing() { return true; }\n"
           "  uint8_t undocumented = 0;\n};")
    assert [i for i in _hdr(src) if "public variable has no ///" in i[1]]


def test_a_public_member_after_a_nested_type_is_still_seen():
    """A nested type opens its own body, and the enclosing class resumes when it closes.
    Tracking one depth instead of a stack meant the nested struct overwrote the class that
    held it, so closing the struct read as closing the class and every public member after
    one escaped this check: a class with a nested type reported nothing at all."""
    nested = ("class Foo {\npublic:\n"
              "  /// documented\n"
              "  void before() {}\n"
              "  /// a nested type\n"
              "  struct Inner { uint8_t a; };\n"
              "  void after();\n"
              "  uint8_t alsoAfter = 0;\n};")
    whys = [why for _, why in _hdr(nested) if "has no ///" in why]
    assert "public function has no ///" in whys
    assert "public variable has no ///" in whys


def test_a_private_nested_type_stays_exempt():
    """The stack must not undo what it replaced: a type declared after `private:` is private
    however it is spelled, and its fields never reach the generated page."""
    src = ("class Foo {\npublic:\n"
           "  /// documented\n"
           "  void ok() {}\n"
           "private:\n"
           "  struct Hidden { uint8_t undocumentedField; };\n"
           "  uint8_t alsoPrivate_ = 0;\n};")
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_friend_declaration_is_not_a_member():
    """`friend class X;` grants access rather than declaring anything: it reaches no generated
    page, so asking it for a `///` would document a line no reader ever sees."""
    src = ("class Foo {\npublic:\n"
           "  /// documented\n"
           "  void ok() {}\n"
           "  friend class ScratchBufferBase;\n};")
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_function_local_type_is_exempt():
    """A struct declared inside a METHOD body cannot be named outside it and never reaches the
    generated page, so demanding a `///` on its fields asks the file to document what no reader
    sees. The same reason `friend` and a private nested type are already exempt."""
    src = ("class Foo {\npublic:\n"
           "  /// documented\n"
           "  bool go() {\n"
           "    struct Local { uint8_t* out; void emit(int x) {} };\n"
           "    return true;\n"
           "  }\n};")
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_class_body_nested_type_still_reports():
    """The exemption must not swallow the case the stack was built for: a type declared
    directly in the class body IS on the generated page, one level below it rather than two."""
    src = ("class Foo {\npublic:\n"
           "  /// documented\n"
           "  void ok() {}\n"
           "  /// a nested type\n"
           "  struct Inner {\n"
           "    uint8_t undocumentedField;\n"
           "  };\n};")
    whys = [why for _, why in _hdr(src) if "has no ///" in why]
    assert "public variable has no ///" in whys


def test_a_member_after_a_function_local_type_is_still_seen():
    """The local frame must end with the method that held it: a public member declared after
    one is an ordinary member, and skipping it would hide the whole rest of the class."""
    src = ("class Foo {\npublic:\n"
           "  /// documented\n"
           "  bool go() {\n"
           "    struct Local { uint8_t hidden; };\n"
           "    return true;\n"
           "  }\n"
           "  uint8_t afterTheMethod = 0;\n};")
    whys = [why for _, why in _hdr(src) if "has no ///" in why]
    assert "public variable has no ///" in whys


def test_the_headers_are_actually_scanned():
    """A rule that reads no files reports a clean run, which looks exactly like a clean
    tree. The count is pinned so an empty scan fails here instead of going quiet."""
    import check_docgen
    assert len(list(check_docgen._headers())) > 10


def test_a_header_a_build_generates_is_not_counted():
    """build_info.h is written by every build and never committed, so counting it moved the
    ratchet with whatever was built last."""
    import check_docgen
    scanned = {p.as_posix() for p in check_docgen._headers()}
    assert "src/core/util/build_info.h" not in scanned
    assert "src/core/system/HttpServerModule.h" in scanned


def test_the_scan_covers_cpp_as_well_as_headers():
    """The same `///` and `//` comments live on both sides of a header/source split, so a
    scan that stopped at `.h` left every `.cpp` silently exempt. Both suffixes are pinned
    here because the count test above passes just as happily with one of them missing."""
    import check_docgen
    scanned = {p.suffix for p in check_docgen._headers()}
    assert scanned == {".h", ".cpp"}, scanned


def test_a_cpp_file_is_held_to_the_comment_rules():
    """A `.cpp` gets no generated page, so only the comment-shape rules reach it. This pins
    that they DO reach it: the rule fires on a source file, not just on a header."""
    import check_docgen
    src = ("class X {\n"
           "public:\n"
           "    /// Does the thing.\n"
           "    void f() {\n"
           + "".join(f"        // line {i}\n" for i in range(9)) +
           "    }\n"
           "};")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.cpp", src)]
    assert any("code comment 9 lines > 1" in w for w in whys), whys


def test_a_cpp_is_asked_for_no_member_docs():
    """`public` is read from a declaration's SHAPE, which cannot see `namespace {` or a function
    body. A header's declarations mostly ARE the public API and each becomes a page row, so the
    approximation holds there. In a `.cpp` it matched fields of file-local structs and plain
    locals, asking a variable named `got` for a doc comment, so the rule stops at the header."""
    import check_docgen
    src = ("struct State {\n"
           "    bool busy = false;   // a frame is on the wire\n"
           "};")
    cpp = [w for _, w in check_docgen._header_rules("src/core/x.cpp", src) if "public variable" in w]
    assert not cpp, cpp
    hdr = [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "public variable" in w]
    assert hdr, hdr


def test_one_rule_set_applies_to_both_kinds_of_file():
    """A `.cpp` follows the same rules as a `.h`; only the SEVERITY differs. Three rules were
    scoped away from implementation files on reasons that did not hold: a `.cpp` carries a file
    lead with its own `@technical` appendix, declares classes, and is read as source like any
    other file. Scoping them back out is how the two columns drift apart again."""
    import check_docgen
    src = ("#include <x.h>\n"
           "\n"
           "void g();\n"
           "\n"
           "/// A two-line note\n"
           "/// about the thing below.\n"
           "void f();\n")
    for rel in ("src/core/x.h", "src/core/x.cpp"):
        whys = [w for _, w in check_docgen._header_rules(rel, src) if "member comment" in w]
        assert whys, (rel, "the member budget applies to both")


def test_a_list_is_structure_rather_than_an_over_long_run():
    """A numbered item is a bullet that starts with a digit, so it is layout exactly as `-` is.
    Counted as prose, an enumerated spec can satisfy the one-line cap only by being fused onto
    one line, which destroys the layout that carries its meaning: a five-item wire-format spec
    became one 299-character line that way. The prose lines in a run still count, so an essay
    with a list in it is measured on the essay."""
    import check_docgen
    lead = "#include <x.h>\n\nvoid g();\n\n"
    lst = ("// Pins three things:\n"
           "//   1. the first\n"
           "//   2. the second\n"
           "//   3. the third\n"
           "void f();\n")
    whys = [w for _, w in check_docgen._header_rules("src/core/x.cpp", lead + lst)]
    assert not [w for w in whys if "code comment" in w], whys
    assert not [w for w in whys if "hard wrap" in w], whys

    essay = lead + "".join(f"// prose line {i}.\n" for i in range(5)) + "void f();\n"
    runs = [w for _, w in check_docgen._header_rules("src/core/x.cpp", essay) if "code comment" in w]
    assert runs == ["code comment 5 lines > 1"], runs


def test_a_lead_is_measured_as_a_block_so_chopping_does_not_satisfy_it():
    """The line cap alone is satisfiable by splitting one long line into two shorter ones, which
    leaves the text identical and every rule passing. A run is what a reader takes in before the
    next declaration, so it carries the same budget over the whole block: line count times line
    length. Pinned because the chopping move is the cheapest way to make this check green, and a
    check that rewards it measures the wrong thing."""
    import check_docgen
    # Twelve lines of 240 characters: every LINE is inside the cap, the run is not.
    lead = "\n".join("/// " + "x" * 236 for _ in range(12))
    src = lead + "\n\nvoid f();\n"
    whys = [w for _, w in check_docgen._header_rules("src/core/x.h", src)]
    assert any("lead" in w and "chars" in w for w in whys), whys
    assert not any("doc line" in w for w in whys), \
        "no single line is over the cap, which is the point of measuring the block"


def test_the_ratchet_refuses_a_rule_that_rose_against_the_committed_report(monkeypatch):
    """A warning is staged work, and staged work that grows is not a sweep. The committed report
    is the number to beat, per RULE and on the total: per rule alone misses a rule still under
    its own baseline while the total rises, and the total alone hides one rule paying for
    another, which is what splitting a long line does to the width and block counts.

    Drives `_ratchet` itself. An earlier version of this test asserted against a local copy of the
    comparison and passed while the real function compared internal keys against display labels,
    so it matched nothing and the per-rule half never fired."""
    import check_docgen
    base = {"(total)": 10, "multi-line comment blocks": 4, "hard wraps": 2}
    monkeypatch.setattr(check_docgen, "_committed_counts", lambda: base)

    # A `.cpp` key, because the ratchet guards the WARNING column and a header's finding blocks.
    cpp = "src/core/x.cpp"
    block = "code comment 3 lines > 1"

    assert check_docgen._ratchet([(cpp, block)] * 3) == [], "under every baseline: silent"

    rose = check_docgen._ratchet([(cpp, block)] * 5)
    assert rose == [("multi-line comment blocks", 4, 5)], \
        f"one rule paying for another is caught, and the total (5) stays under 10: {rose}"

    # The labels the report writes ARE the keys the comparison uses, in both directions.
    assert check_docgen._rule_label(check_docgen._rule_name(block)) in base

    # And the real parser finds the tracked report's numbers, or the comparison reads nothing.
    monkeypatch.undo()
    counts = check_docgen._committed_counts()
    assert counts is None or "(total)" in counts, counts
    # Only the `## By rule` table feeds the baseline: an area row would be a rule that never appears.
    assert counts is None or not any(k.startswith("`") for k in counts), counts


def test_the_appendix_is_measured_from_technical_to_the_end_of_its_run():
    """The appendix is what a file holds outside its code: every `///` line after `@technical`, up to the
    first line that is not `///`. A lead without one measures zero, so a file with no appendix is not counted."""
    import check_docgen
    text = ("/// Lead.\n///\n/// @technical\n///\n/// ## Why\n///\n/// One fact.\n"
            "#include <x>\n/// A member comment, not appendix.\nint x;\n")
    assert check_docgen._appendix_lines(text) == 4
    assert check_docgen._appendix_lines("/// Lead only.\nint x;\n") == 0
    # A marker after the summary text opens the appendix too, as it does for the lead rules.
    assert check_docgen._appendix_lines("/// Lead. @technical\n/// ## Why\n/// One fact.\nint x;\n") == 2


def test_the_ratchet_refuses_an_appendix_that_grew(monkeypatch):
    """Warnings that fall while the appendix grows were moved, not cut, so the appendix only shrinks too.
    A baseline written before the number existed carries no row, and a first run must not fail on that."""
    import check_docgen
    monkeypatch.setattr(check_docgen, "_committed_counts", lambda: {"(total)": 10, "(appendix lines)": 100})
    assert check_docgen._ratchet([], 100) == [], "held: silent"
    assert check_docgen._ratchet([], 90) == [], "cut: silent"
    assert check_docgen._ratchet([], 120) == [("(appendix lines)", 100, 120)]
    monkeypatch.setattr(check_docgen, "_committed_counts", lambda: {"(total)": 10})
    assert check_docgen._ratchet([], 120) == [], "no baseline yet: the first run records it"


def test_the_ratchet_catches_a_rule_the_baseline_never_had(monkeypatch):
    """A rule at zero is ABSENT from the report, so the baseline has no row for it. Iterating the
    baseline alone let such a rule rise silently while the total fell, which is the same hole the
    per-rule half exists to close. The comparison is over the union, a missing baseline reading
    as zero."""
    import check_docgen
    base = {"(total)": 10, "multi-line comment blocks": 4}
    monkeypatch.setattr(check_docgen, "_committed_counts", lambda: base)

    cpp = "src/core/x.cpp"
    # A warning-side rule with no baseline row. (A hard wrap cannot serve here: it blocks
    # everywhere by design, so it is an error and never enters the warning column.)
    fresh = "file opens with // rather than ///"
    assert check_docgen._rule_label(check_docgen._rule_name(fresh)) not in base

    rose = check_docgen._ratchet([(cpp, fresh)] * 5)
    assert rose == [("files opening with //", 0, 5)], \
        f"a rule absent from the baseline must still only shrink: {rose}"


def test_a_hard_wrap_blocks_in_an_implementation_file_too():
    """The one rule staged the OTHER way. Every other `.cpp` finding warns while the sweep runs,
    but the tree is at ZERO hard wraps, so there is nothing left to stage and the exemption only
    protects a regression. The reason holds everywhere too: a split sentence reflows every line
    it spans on the next word change, which costs a reviewer the same in either file."""
    import check_docgen
    src = ("#include <x.h>\n\nvoid g();\n\n"
           "// A sentence that carries on\n"
           "// onto the following line.\n"
           "void f();\n")
    for rel in ("src/core/x.h", "src/core/x.cpp"):
        hits = [(k, w) for k, w in check_docgen._header_rules(rel, src) if "hard wrap" in w]
        assert hits, rel
        assert all(check_docgen._blocks(k, w) for k, w in hits), (rel, "must BLOCK")
    # And the rules still being swept keep warning in a `.cpp`, or the staging is gone entirely.
    runs = [(k, w) for k, w in check_docgen._header_rules("src/core/x.cpp", src)
            if "code comment" in w]
    assert runs and not any(check_docgen._blocks(k, w) for k, w in runs), runs


def test_the_line_length_cap_warns_even_in_a_header():
    """The one staged exception. Every other rule describes how the tree is already written, so
    a finding is a defect against a standard the file was written under. This cap is new, so its
    findings are lines nobody wrote wrongly, and erroring on them would stop commits over a
    target rather than a defect. It becomes an error once the tree meets it."""
    import check_docgen
    long_line = "/// " + "word " * 60
    assert len(long_line) > check_docgen.MAX_COMMENT_LINE_CHARS
    src = "#include <x.h>\n\nvoid g();\n\n" + long_line + "\nvoid f();\n"
    hits = [(k, w) for k, w in check_docgen._header_rules("src/core/x.h", src) if "chars >" in w]
    assert hits, "a header's over-long line is still REPORTED"
    assert not any(check_docgen._blocks(k, w) for k, w in hits), \
        "but it warns rather than blocking, unlike every other header finding"
    # A header finding of any OTHER kind still blocks, or the exception has swallowed the rule.
    other = [(k, w) for k, w in check_docgen._header_rules("src/core/x.h", src) if "chars >" not in w]
    assert any(check_docgen._blocks(k, w) for k, w in other), other


def test_a_local_struct_in_an_allman_function_is_not_a_member():
    """A free function's body is tracked by the brace that opens it, wherever that brace sits.
    With the brace on its own line the signature and the body open on different lines, so a
    local struct declared inside could be mistaken for a documented type's member and its
    fields asked for a `///` they do not owe. Both brace styles are pinned, and a genuine
    member after the function still reports, so the exclusion cannot swallow a real finding."""
    import check_docgen
    body = ("/// doc\nint f()%s\n"
            "    struct Local { int x; };\n    return sizeof(Local);\n}\n"
            "struct S {\n    int shouldBeFlagged;\n};\n")
    for brace in ("\n{", " {"):
        whys = [w for k, w in check_docgen._header_rules("src/core/x.h", body % brace)]
        assert whys == ["public variable has no ///"], (brace, whys)


def test_a_header_finding_blocks_and_an_implementation_one_warns():
    """Severity follows the same split `_generates_a_page` draws. A header's comments ARE the
    published page, so a finding there is a defect in what ships and fails the gate. An
    implementation file publishes nothing, so an over-long note to the next reader is worth
    fixing without holding a commit. A catalog page publishes too, so it blocks like a header.
    What the split must NOT do is hide the warnings: they stay in the report and in the count,
    because the split stages the sweep rather than ranking the two kinds of comment, and it goes
    once the warning side reaches zero."""
    import check_docgen
    assert check_docgen._blocks("src/core/x.h::Thing")
    assert check_docgen._blocks("docs/moonmodules/light/effects.md::SomeCard")
    assert not check_docgen._blocks("src/core/x.cpp::line 12")


def test_a_provenance_marker_above_the_lead_is_not_the_lead():
    """A file's lead is the first thing that DOCUMENTS it. An SPDX tag is machine-read and an
    `Author:` line is a credit, so both sit above the real lead and neither is one: counted as the
    lead they reported every licensed or credited header as opening with `//` while a proper `///`
    lead stood two lines below. A forward declaration names a type defined elsewhere and documents
    nothing either. What none of them may do is stand in for a lead that is missing."""
    import check_docgen
    def whys(src):
        return [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "lead" in w]

    for marker in ("// SPDX-License-Identifier: GPL-3.0-or-later",
                   "// Author: MoonLight original"):
        src = f"{marker}\n#pragma once\n/// One thing.\nclass Thing {{\npublic:\n    /// Go.\n    void go();\n}};\n"
        assert not whys(src), marker

    fwd = "#pragma once\nclass Other;\n/// One thing.\nclass Thing {\npublic:\n    /// Go.\n    void go();\n};\n"
    assert not whys(fwd), "a forward declaration is not a lead"

    bare = "// Author: MoonLight original\n#pragma once\nclass Thing {\npublic:\n    void go();\n};\n"
    assert whys(bare), "a marker must not stand in for a missing lead"


def test_an_xref_that_cannot_render_is_reported():
    """`@xref{anchor|label}` renders to a same-page link, and the generator resolves `[a-z0-9-]+`
    only. An underscore therefore never becomes a link: the marker survives Doxygen untouched and
    the reader sees raw `@xref{...}` mid-sentence. Matching that same narrow class HERE made the
    rule silent on exactly the names that cannot work, so it reported a clean run over 21 of them
    — a check that read nothing is indistinguishable from a check that found nothing. So anything
    `@xref{...}`-shaped is matched loosely and then judged: a name that is not a slug is
    unrenderable, and a slug naming no heading in the file is dead."""
    import check_docgen
    def whys(src):
        return [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "xref" in w]

    HEAD = ("/// A lead.\n///\n/// @technical\n///\n"
            "/// ## A real heading\n///\n")
    TAIL = "inline void f();\n"
    assert any("renders as raw text" in w
               for w in whys(HEAD + "/// See @xref{some_anchor|this}.\n" + TAIL)), \
        "an underscore cannot render, so it must be reported"
    assert any("names no heading" in w
               for w in whys(HEAD + "/// See @xref{no-such-heading|this}.\n" + TAIL)), \
        "a slug naming no heading is dead"
    assert not whys(HEAD + "/// See @xref{a-real-heading|this}.\n" + TAIL)
    # The label separator may be written escaped, which the generator accepts, so the backslash
    # lands in the captured anchor and must not be read as part of the slug.
    assert not whys(HEAD + "/// See @xref{a-real-heading\\|this}.\n" + TAIL), \
        "an escaped pipe is a valid reference"


def test_a_cpp_may_open_with_a_plain_comment_but_must_open_with_one():
    """The SPELLING of a file lead is a header's business, because a header's lead is its
    generated page's opening paragraph and `//` generates nothing. An implementation file has no
    page, so there `//` is a note to the next reader and reads correctly. What both owe is a lead
    at all: a file that opens on code says nothing about itself to anybody."""
    import check_docgen
    plain, none_ = "// what this file is for\nint f() { return 1; }\n", "#include <x>\nint f() { return 1; }\n"
    for rel in ("src/core/x.cpp", "src/core/x.h"):
        assert [w for _, w in check_docgen._header_rules(rel, plain) if "opens with //" in w], rel
    for rel in ("src/core/x.cpp", "src/core/x.h"):
        assert [w for _, w in check_docgen._header_rules(rel, none_) if "file lead missing" in w], rel


def test_a_cpp_is_asked_for_no_member_docs_of_either_kind():
    """Functions go the same way as variables, for the same reason: what the rule matched in a
    `.cpp` was the constructor or destructor of a file-local RAII struct — `~WinsockInit`,
    `Lock`, `ParkGuard`: none of them public API, each already explained by the struct's own
    comment. A header keeps both halves, where a declaration really is the public surface."""
    import check_docgen
    src = ("struct State {\n"
           "    void start();\n"
           "    bool busy = false;\n"
           "};")
    cpp = [w for _, w in check_docgen._header_rules("src/core/x.cpp", src)
           if "has no ///" in w]
    assert not cpp, cpp
    hdr = [w for _, w in check_docgen._header_rules("src/core/x.h", src)
           if "has no ///" in w]
    assert any("public function has no ///" in w for w in hdr), hdr
    assert any("public variable has no ///" in w for w in hdr), hdr


def test_a_fenced_block_is_not_read_as_wrapped_prose():
    """Skipping the ``` delimiters alone was not enough. A fence's whole point is that what sits
    BETWEEN them is verbatim, so a wire table was flagged on every row: each line ends without a
    full stop, which is exactly what the no-wrap rule looks for. The same exemption the appendix
    section cap already makes, and the one the standards state."""
    import check_docgen
    src = (""
           "/// A lead line.\n"
           "///\n"
           "/// ```\n"
           "/// div 3  26.67 MHz  zero 300 ns   strict spec, but strands scramble\n"
           "/// div 4  20.00 MHz  zero 400 ns   the reliability point\n"
           "/// ```\n"
           ""
           "void f();\n")
    whys = [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "hard wrap" in w]
    assert not whys, whys


def test_prose_after_a_closed_fence_is_still_checked():
    """The exemption tracks the fence STATE rather than switching off at the first one, so a
    wrapped sentence below a closed block is caught as it always was."""
    import check_docgen
    src = (""
           "/// A lead line.\n"
           "///\n"
           "/// ```\n"
           "/// a table row\n"
           "/// ```\n"
           "///\n"
           "/// This sentence is carried\n"
           "/// onto the next line.\n"
           ""
           "void f();\n")
    whys = [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "hard wrap" in w]
    assert whys, whys


def test_one_comment_cap_holds_in_both_kinds_of_file():
    """A `//` run gets one line wherever it sits. Depth is not forbidden but HOMED: a header
    carries it in an `@technical` appendix and an implementation file carries it in the same
    appendix on its own file lead, so the escape hatch exists in both. A larger cap in a `.cpp`
    was an attempt to home depth inline, which leaves the reasoning beside one call rather than
    where a reader goes looking for it."""
    import check_docgen
    body = ("class X {\n"
            "public:\n"
            "    /// Does the thing.\n"
            "    void f() {\n"
            + "".join(f"        // line {i}\n" for i in range(4)) +
            "    }\n"
            "};")
    for rel in ("src/core/x.h", "src/core/x.cpp"):
        whys = [w for _, w in check_docgen._header_rules(rel, body) if "code comment" in w]
        assert any("code comment 4 lines > 1" in w for w in whys), (rel, whys)


def test_the_scan_reaches_beyond_src():
    """The entry points and the ISA generator sit outside `src` by build-system convention
    alone, and were exempt for that reason rather than by decision. Pinned by root so a
    dropped one fails here instead of quietly shrinking the measured surface."""
    import check_docgen
    scanned = {p.as_posix() for p in check_docgen._headers()}
    for expected in ("esp32/main/main.cpp",
                     "moonbase/main/moonbase_main.cpp",
                     "moondeck/moonlive/emit_isa.cpp"):
        assert expected in scanned, expected


def test_the_file_comment_is_exempt_but_nothing_below_it_is():
    """The file comment is the one `//` block at the top, before anything is declared. The rule
    used to exempt everything above the first `class`, which in a header of free functions is
    most of the file: 1002 multi-line runs across 149 files sat unmeasured on the declarations
    they documented. Both halves are pinned, since exempting nothing is the opposite mistake."""
    import check_docgen
    src = ("// The file comment.\n"
           "// Its second line, which is allowed.\n"
           "\n"
           "#include <cstdint>\n"
           "\n"
           "// A comment on a declaration,\n"
           "// spanning two lines, which is not.\n"
           "using Fn = void (*)(int);\n")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.h", src)]
    assert [w for w in whys if "code comment 2 lines" in w], whys
    assert len([w for w in whys if "code comment" in w]) == 1, whys


def test_a_file_lead_written_in_paragraphs_stays_exempt():
    """A lead is written in paragraphs, and a blank line between two of them does not end it.
    Stopping at the first blank exempted only the opening paragraph and reported the rest as
    essays sitting on the next declaration, when they are the file's documentation in the only
    place a `.cpp` has for it. The `@module`/`@also` lead of unit_Effects_golden.cpp is the
    case: 33 lines that no appendix could hold, because they ARE the lead."""
    import check_docgen
    src = ("// The first paragraph of the lead.\n"
           "//\n"
           "// The second, after a blank line inside the comment.\n"
           "\n"
           "// A third, after a blank line BETWEEN blocks, still the lead:\n"
           "// nothing has been declared yet.\n"
           "\n"
           "#include <cstdint>\n"
           "\n"
           "void f();\n"
           "\n"
           + "".join(f"// essay line {i}\n" for i in range(9)) +
           "void g();\n")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.cpp", src)]
    runs = [w for w in whys if "code comment" in w]
    assert runs == ["code comment 9 lines > 1"], runs


def test_a_file_with_no_leading_comment_exempts_nothing():
    """`_file_comment_end` answers 0 once a declaration has been seen, so a file that opens with
    code buys no exemption for a comment further down. Otherwise a file could dodge the rule by
    omitting its own header comment, which is the wrong incentive.

    The file comment may sit BELOW the includes, which is where this repo puts it, so the
    fixture has to declare something first to prove the other half."""
    import check_docgen
    src = ("#include <cstdint>\n"
           "using Fn = void (*)(int);\n"
           "// Two lines,\n"
           "// on a later declaration.\n"
           "using G = int;\n")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.h", src)]
    assert [w for w in whys if "code comment 2 lines" in w], whys


def test_a_templated_member_is_not_asked_for_a_second_doc():
    """Doxygen attaches a comment to the declaration, and a `template <...>` header sits
    between the two. Reading only the line above asked a documented templated member for a
    `///` it already had, and the answer was a second one wedged under the template header,
    which doxygen drops and a reader sees twice."""
    import check_docgen
    src = ("class X {\n"
           "public:\n"
           "    /// Walks the thing.\n"
           "    template <typename F>\n"
           "    void walk(F&& f) const { (void)f; }\n"
           "};")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.h", src)]
    assert not [w for w in whys if "has no ///" in w], whys


def test_a_multiline_struct_inside_a_free_function_is_exempt():
    """The one-line form was already exempt, but a struct whose fields sit on their own lines
    reached the member scan and reported `public variable has no ///`. A free function has no
    enclosing class, so the frame stack is empty and the depth test had nothing to compare
    against: locality there is the function body's own depth."""
    import check_docgen
    src = ("namespace mm {\n"
           "\n"
           "void f() {\n"
           "    struct Guard {\n"
           "        int fd;\n"
           "    };\n"
           "    Guard g{0};\n"
           "    (void)g;\n"
           "}\n"
           "\n"
           "}\n")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.h", src)]
    assert not [w for w in whys if "has no ///" in w], whys


def test_a_namespace_level_struct_is_still_public():
    """The free-function exemption must not swallow an ordinary type: one declared at namespace
    scope reaches the generated page and still owes a `///`."""
    import check_docgen
    src = ("namespace mm {\n"
           "\n"
           "struct Public {\n"
           "    int field;\n"
           "};\n"
           "\n"
           "}\n")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.h", src)]
    assert [w for w in whys if "has no ///" in w], whys


def test_a_struct_inside_a_free_function_is_exempt():
    """A type declared in a free function's body reaches no reader: it has no enclosing class,
    so the frame stack is empty and the old rule read it as a top-level public member. With
    `.cpp` files in scope that shape is ordinary (a scoped RAII helper), so it is pinned."""
    import check_docgen
    src = ("void f() {\n"
           "    struct Guard { int fd; };\n"
           "    Guard g{0};\n"
           "    (void)g;\n"
           "}")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.cpp", src)]
    assert not [w for w in whys if "has no ///" in w], whys


def test_every_source_folder_maps_to_a_summary_page():
    """The report groups findings by the page a sweep runs against, so a folder that maps to
    nothing would silently vanish from that view. Pinned per domain folder: the mapping is the
    machine-readable half of the hierarchy zoom in documentation-standards.md."""
    import check_docgen, pathlib
    root = pathlib.Path(check_docgen.ROOT)
    for domain in ("core", "light"):
        for folder in sorted((root / "src" / domain).iterdir()):
            if not folder.is_dir():
                continue
            rel = f"src/{domain}/{folder.name}/X.h"
            area = check_docgen._doc_area(rel)
            assert area.endswith(".md"), f"{rel} -> {area}"


def test_a_path_outside_the_card_surface_is_bucketed_not_lost():
    """The tests carry no card by design, so they get a named bucket rather than a summary
    page. Anything else is `(unassigned)`, which is the finding: a header no page owns
    reaches no reader. The platform layer USED to sit in a bucket here and now has its own
    page, which is what a bucket is for: naming the gap until it is filled."""
    import check_docgen
    assert check_docgen._doc_area("src/platform/platform.h") == "platform/index.md"
    assert check_docgen._doc_area("test/unit/core/x.cpp") == "(tests, no card)"
    # A path under no DOC_AREAS prefix at all. `moonbase/main` stood here until it was
    # assigned to core/system.md, which is the bucket working as intended rather than a
    # regression: an example that gets a page is replaced, not restored.
    assert check_docgen._doc_area("tools/nowhere/x.cpp") == "(unassigned)"


def test_the_scan_includes_tests():
    """A test is C++ we own, and these rules are about how code READS rather than about what
    gets published, so `test/` is in scope like any other source. Pinned because excluding it
    is the easy mistake: nothing generates a page from a test, which looks like a reason."""
    import check_docgen
    assert [p for p in check_docgen._headers() if p.as_posix().startswith("test/")]


def test_a_test_hook_needs_no_doc():
    """A test hook is named `...ForTest`, which says what it is, so a `///` on it adds nothing. The exemption covers functions, a hook type's constructor and its destructor,
    and stops there: an ordinary undocumented member beside them is still reported."""
    src = ("class Foo {\npublic:\n"
           "  void injectForTest();\n"
           "  struct LookupForTest {\n"
           "    explicit LookupForTest(int resolve);\n"
           "    ~LookupForTest();\n"
           "  };\n"
           "  void other();\n};")
    flagged = [i for i in _hdr(src) if "has no ///" in i[1]]
    assert len(flagged) == 1, flagged   # only `other`


# ---- appendix sections nothing refers to ----

_APPENDIX = ["/// The thing.", "///", "/// @technical", "///", "/// ## Why it waits",
             "///", "/// Because.", "///", "/// ## The wire layout", "///", "/// Bytes."]


def test_an_appendix_section_nothing_refers_to_is_a_finding():
    from check_docgen import _unreferenced_in
    found = _unreferenced_in("src/core/x.h", _APPENDIX)
    assert [k for k, _ in found] == ["src/core/x.h::@technical Why it waits", "src/core/x.h::@technical The wire layout"]


def test_an_xref_in_the_file_reaches_a_section():
    from check_docgen import _unreferenced_in
    lines = _APPENDIX + ["/// See @xref{why-it-waits}."]
    assert [k for k, _ in _unreferenced_in("src/core/x.h", lines)] == ["src/core/x.h::@technical The wire layout"]


def test_a_heading_inside_a_fenced_example_is_no_section():
    from check_docgen import _unreferenced_in
    lines = ["/// The thing.", "///", "/// @technical", "///", "/// ## Real", "///", "/// ```text",
             "/// ## not a section", "/// ```"]
    assert [k for k, _ in _unreferenced_in("src/core/x.h", lines)] == ["src/core/x.h::@technical Real"]


def test_without_llvm_the_help_freshness_is_reported_rather_than_ending_the_run(monkeypatch):
    """A contributor without LLVM still gets the whole report, with one failing line naming the check it could not make."""
    import check_docgen
    import gen_help
    monkeypatch.setattr(gen_help, "stale", lambda: (_ for _ in ()).throw(SystemExit("clang not found")))
    findings = check_docgen._help_freshness()
    assert findings == [("docs/help/help.json", "help.json stale unchecked: clang not found")]
    assert check_docgen._blocks(*findings[0])


def test_a_control_guide_repeating_its_line_is_reported_and_one_adding_to_it_is_not():
    import check_docgen
    help_file = {"modules": {"DemoEffect": {"source": "https://github.com/MoonModules/MoonLight/blob/main/src/light/effects/DemoEffect.h", "controls": {
        "persistence": {"line": "How long light survives in the flow, as a half-life.", "more": "How long light survives once it is in the flow, as a half-life."},
        "fit": {"line": "Keep the aspect with bands or cropped, or stretch it to the grid.", "more": "**fit** shows the whole picture, with bands where its aspect leaves the grid empty."},
        "formation": {"line": "How the heads relate: fan, mirror, chase, cross or unison.", "more": "How the heads relate. With fan the beams open like a hand."}}}}}
    findings = check_docgen._guide_repeats(help_file)
    assert [f[1].split("`")[1] for f in findings] == ["persistence", "formation"]   # the guide saying the line, and opening with part of it


def test_the_app_help_fits_a_glance_per_control_per_section_and_in_total():
    """A control's longer help shows inline under it, a guide section under the arrow: each is capped in words, and so is the whole, so tables and steps go to a how-to page."""
    long = " ".join(["word"] * 70)
    help_file = {"catalogs": {"light/demo.md": {}},
                 "modules": {"DemoDriver": {"doc": "light/demo.md#demo", "source": "https://x/blob/main/src/light/DemoDriver.h",
                                            "summary": "", "controls": {"pins": {"line": "The pins.", "more": long}},
                                            "guide": [{"title": "Wiring", "text": long}, {"title": "Tips", "text": "Short."}]},
                             "OtherModule": {"doc": "core/other.md#other", "controls": {"x": {"more": long}}}}}
    import check_docgen
    found = check_docgen._help_size(help_file)
    assert ("src/light/DemoDriver.h", "app help too long: `pins`'s guide 70 words > 40") in found
    assert ("src/light/DemoDriver.h", "app help too long: guide 'Wiring' 70 words > 60") in found
    assert not any("Tips" in msg or "> 250" in msg for _, msg in found)   # 141 words in all
    help_file["modules"]["DemoDriver"]["summary"] = long + " " + long
    assert any(msg.startswith("app help too long: 281 words > 250") for _, msg in check_docgen._help_size(help_file))
    assert not any("other" in src for src, _ in found)   # a page still written by hand is not measured
    help_file["modules"]["DemoDriver"]["summary"] = ""
    help_file["modules"]["DemoDriver"]["guide"] = [{"title": "Wiring", "text": long}, {"title": "Wiring", "text": long}]
    # Two sections sharing a title each count, toward the total too.
    assert sum("guide 'Wiring'" in msg for _, msg in check_docgen._help_size(help_file)) == 2


def test_a_control_is_explained_on_its_member_never_in_the_module_guide():
    import check_docgen
    help_file = {"modules": {"DemoDriver": {"source": "https://x/blob/main/src/light/DemoDriver.h", "controls": {"pins": {"line": "The pins."}},
                                            "guide": [{"title": "pins", "text": "Wire them."}, {"title": "Wiring", "text": "Short."}]}}}
    assert check_docgen._guide_names_control(help_file) == [
        ("src/light/DemoDriver.h", "module guide explains a control `pins`: move it to the member's `/// @guide` line")]


def test_every_module_and_every_control_has_the_line_the_app_shows():
    import check_docgen
    help_file = {"modules": {"DemoDriver": {"source": "https://x/blob/main/src/light/DemoDriver.h", "line": "Drives a demo.",
                                            "controls": {"pins": {"line": "The pins."}, "bare": {"line": ""}}},
                             "Silent": {"source": "https://x/blob/main/src/light/Silent.h"}}}
    assert check_docgen._help_lines(help_file) == [
        ("src/light/DemoDriver.h", "help line missing: control `bare` has no `///` on its member or `add…` line"),
        ("src/light/Silent.h", "help line missing: `Silent` has no class `///`")]


def test_a_tag_emoji_groups_at_least_two_modules_or_scripts():
    """A tag is a filter chip, so one carrier groups nothing; a script sharing it counts, and ⚡️ matches ⚡ whatever its variation selector."""
    import check_docgen
    help_file = {"modules": {"Fire": {"source": "https://x/blob/main/src/light/Fire.h", "tags": "⚡️🎯"},
                             "Noise": {"source": "https://x/blob/main/src/light/Noise.h", "tags": "⚡🌊"}}}
    assert check_docgen._tag_groups(help_file, {"moonlive/effects/aim.mle": "🎯"}) == [
        ("src/light/Noise.h", "tag emoji on one module: 🌊 groups nothing; share one from the legend or drop it")]



def _catalog(page: str, **module) -> dict:
    """A help file holding one module on catalog `page`, as gen_help writes it."""
    entry = {"source": "https://x/blob/main/src/light/Demo.h", "doc": f"{page}#demo", "controls": {}}
    entry.update(module)
    return {"catalogs": {page: {}}, "modules": {"DemoEffect": entry}}


def test_a_catalog_card_leads_with_its_picture_and_a_moving_one_is_a_gif():
    """Effects, modifiers and layouts show motion as a gif, the other catalogs a png, and a card without a picture is reported."""
    import check_docgen
    whys = lambda h: [w for _, w in check_docgen._help_cards(h)]
    assert any(w.startswith("no image") for w in whys(_catalog("light/effects.md")))
    assert any(w.startswith("image is png, not a gif") for w in whys(_catalog("light/effects.md", image="a.png")))
    assert any(w.startswith("image is gif, not a png") for w in whys(_catalog("light/drivers.md", image="a.gif")))
    assert whys(_catalog("light/effects.md", image="a.gif")) == []
    assert whys(_catalog("light/supporting.md")) == []   # a summary page's rows describe machinery with no card to capture


def test_a_control_line_too_wide_for_its_card_is_reported_tighter_on_a_visual_catalog():
    import check_docgen
    line = "x" * 80
    controls = {"speed": {"line": line}}
    assert any(w.startswith("one control") for _, w in check_docgen._help_cards(_catalog("light/effects.md", image="a.gif", controls=controls)))
    assert not any(w.startswith("one control") for _, w in check_docgen._help_cards(_catalog("light/drivers.md", image="a.png", controls=controls)))


def test_a_control_stating_its_default_or_range_is_reported():
    """The reset button shows a control's default and the slider its range, so a line stating either restates the UI and drifts when the code changes. A name in backticks is not read: an option called `default` is the option's name."""
    import check_docgen
    controls = {"fps": {"line": "Frames per second (default 24, 1 to 25)."}, "size": {"line": "Square edge in lights (1–64)."}, "level": {"line": "The level, 0-255."},
                "device": {"line": "The input, `default` following the system."}, "name": {"line": "What to call it, up to 15 characters."},
                "speed": {"line": "How fast it turns."}, "chip": {"line": "A WS2812-2020 strip."}}
    findings = check_docgen._help_cards(_catalog("light/drivers.md", image="a.png", controls=controls))
    restating = [why for _, why in findings if why.startswith("control restates the UI")]
    assert len(restating) == 1 and restating[0].endswith(": fps, size, level, name")


def test_the_class_introducing_a_catalog_is_no_card_and_needs_no_picture():
    import check_docgen
    help_file = _catalog("core/services.md")
    help_file["catalogs"]["core/services.md"] = {"base": "DemoEffect"}
    assert check_docgen._help_cards(help_file) == []


def test_a_module_off_the_catalogs_has_no_card_to_measure():
    import check_docgen
    help_file = _catalog("light/effects.md")
    help_file["catalogs"] = {}
    assert check_docgen._help_cards(help_file) == []


def _notes(src: str, rel: str = "src/core/x.h") -> list:
    import check_docgen
    return [k.partition("::")[2] for k, w in check_docgen._header_rules(rel, src) if w.startswith(check_docgen._NOTE_ON_DECL_RULE)]


def test_a_note_on_a_declaration_is_reported_whatever_its_access():
    """A `//` above a declaration, or above its `///`, reaches no reader: the help, the docs and the hover read `///` only."""
    src = ("/// Lead.\nclass Foo {\npublic:\n"
           "    // why it is virtual\n    /// What it returns.\n    virtual int a() const;\n"
           "    // only a note\n    int b = 0;\n"
           "private:\n    // a private note\n    int c_ = 0;\n};\n")
    assert _notes(src) == ["a", "b", "c_"]


def test_a_declaration_with_one_doc_line_a_banner_or_a_body_note_is_clean():
    """A banner separated by a blank line groups members rather than commenting one, and `//` inside a body is a note on a line of code."""
    src = ("/// Lead.\nclass Foo {\npublic:\n"
           "    // ---- Pins ----\n\n    /// The data pin.\n    int pin = 0;\n"
           "    /// Run it.\n    void run() {\n        // why this order\n        int x = 0;\n    }\n};\n")
    assert _notes(src) == []
    assert _notes("/// Lead.\n\n// --8<-- [start:k]\n/// A constant.\nconstexpr int kMax = 4;\n") == []   # a docs snippet marker


def test_a_note_on_a_file_scope_declaration_is_reported_and_an_implementation_file_is_not_measured():
    src = "/// Lead.\n\n// a free function's note\ninline int twice(int v) { return 2 * v; }\n\n/// A constant.\nconstexpr int kMax = 4;\n"
    assert _notes(src) == ["twice"]
    assert _notes(src, "src/core/x.cpp") == []


def test_a_template_line_and_a_provenance_marker_add_no_second_or_false_finding():
    """The note above `template <class T>` is reported once, at the declaration below it, and an SPDX tag is provenance, no comment."""
    assert _notes("/// Lead.\n\n// why\ntemplate <class T>\nclass Foo {};\n") == ["Foo"]
    assert _notes("// SPDX-License-Identifier: MIT\n/// What Foo is.\nclass Foo {};\n") == []


def test_a_run_flush_against_a_declaration_is_its_comment_not_the_file_lead():
    import check_docgen
    assert check_docgen._lead_start("#pragma once\nnamespace mm {\n\n/// The file.\n\nint f();\n".split("\n")) == 3
    assert check_docgen._lead_start("#pragma once\nnamespace mm {\n\n/// What f does.\nint f();\n".split("\n")) is None
