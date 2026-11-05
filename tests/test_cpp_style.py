"""Regression tests for the semantic style checker, including safe fix ranges."""

import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("cpp_style", Path(__file__).resolve().parents[1] / "scripts/cpp_style.py")
style = importlib.util.module_from_spec(spec)
spec.loader.exec_module(style)


class StyleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clang = style.load_clang(style.llvm_prefix())
        cls.index = cls.clang.Index.create()

    def inspect(self, source, production=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            path = root / ("components" if production else "tests") / "case.cpp"
            path.parent.mkdir()
            path.write_text(source)
            unit = self.index.parse(str(path), args=["-std=c++17"])
            errors = [str(d) for d in unit.diagnostics if d.severity >= self.clang.Diagnostic.Error]
            self.assertEqual(errors, [])
            with patch.object(style, "ROOT", root):
                findings, fixes, _ = style.inspect(self.clang, unit, {path})
            content = source.encode()
            for _, offset in sorted(fixes, key=lambda fix: fix[1], reverse=True):
                content = content[:offset] + b"this->" + content[offset:]
            return {(f[3], f[4]) for f in findings}, content.decode()

    def test_implicit_fields_and_methods(self):
        findings, fixed = self.inspect("class A { int x_{}; int f(){return x_;} int g(){return f();} };")
        self.assertEqual(findings, {("explicit-this", "x_"), ("explicit-this", "f")})
        self.assertIn("return this->x_;", fixed)
        self.assertIn("return this->f();", fixed)
        self.assertEqual(self.inspect(fixed)[0], set())

    def test_explicit_receivers_static_and_locals(self):
        findings, _ = self.inspect("""
class A { int x_{}; static int count_; static int s();
int f(A &other) { int x = 1; return this->x_ + other.x_ + x + count_ + s(); }
};""")
        self.assertEqual(findings, set())

    def test_initializers_and_default_member_initializers(self):
        findings, fixed = self.inspect("class A { int x_; int y_ = x_; public: A(int x):x_(x){} };")
        self.assertEqual(findings, {("explicit-this", "x_")})
        self.assertIn("int y_ = this->x_;", fixed)
        self.assertIn(":x_(x)", fixed)

    def test_inherited_methods_and_lambda(self):
        findings, fixed = self.inspect("""
class Base { protected: int f(){return 1;} };
class A : public Base { int g(){auto fn=[this](){return f();}; return fn();} };
""")
        self.assertEqual(findings, {("explicit-this", "f")})
        self.assertIn("return this->f();", fixed)

    def test_template_members(self):
        findings, fixed = self.inspect("template<class T> class A { T value_; T f(){return value_;} };")
        self.assertEqual(findings, {("explicit-this", "value_")})
        self.assertIn("return this->value_;", fixed)

    def test_macro_arguments_do_not_rewrite_macro_name(self):
        findings, fixed = self.inspect("""
#define assert(e) ((e) ? (void)0 : (void)0)
class A { int a{}; int count_{}; void f(){assert(a == count_);} };
""")
        self.assertEqual(findings, {("explicit-this", "a"), ("explicit-this", "count_")})
        self.assertNotIn("this->assert", fixed)
        self.assertIn("assert(this->a == this->count_)", fixed)

    def test_macro_generated_fields_are_checked(self):
        findings, _ = self.inspect("#define FIELD protected: int data_;\nclass A { FIELD };\n")
        self.assertEqual(findings, {("protected-field", "data_")})

    def test_macro_body_is_not_rewritten(self):
        source = "#define READ value_\nclass A { int value_; int f(){return READ;} };\n"
        findings, fixed = self.inspect(source)
        self.assertEqual(findings, {("explicit-this", "value_")})
        self.assertEqual(fixed, source)

    def test_protected_fields_including_static(self):
        findings, _ = self.inspect("class A { protected: int value_; static int count_; void f(); };")
        self.assertEqual(findings, {("protected-field", "value_"), ("protected-field", "count_")})

    def test_public_value_record(self):
        self.assertEqual(self.inspect("struct Value { int number; }; ", production=True)[0], set())

    def test_production_class_state_is_private(self):
        findings, _ = self.inspect(
            "class A { public: int value_; static int count_; static constexpr int LIMIT=4; };", production=True
        )
        self.assertEqual(findings, {("private-class-state", "value_"), ("private-class-state", "count_")})

    def test_test_double_observation_fields(self):
        self.assertEqual(self.inspect("class Fake { public: int calls; }; ")[0], set())


if __name__ == "__main__":
    unittest.main()
