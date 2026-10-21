//! Minimal, strict JSON (RFC 8259) reader and writer.
//!
//! This replaces `serde` + `serde_json` (and their proc-macro build chain:
//! `serde_derive`, `syn`, `quote`, `proc-macro2`, ...). It only implements what
//! the history store needs: a [`Value`] tree, a parser and a serializer.
//!
//! Deliberate properties:
//! * Strict input: no comments, no trailing commas, no raw control characters
//!   in strings, no lone surrogates, no trailing data, nesting depth <= 128.
//! * Duplicate object keys are rejected (avoids parser-differential surprises).
//! * Numbers are kept as their validated source text, so they round-trip
//!   losslessly (no f64 detour for 64-bit integers).
//! * Objects are ordered by key (`BTreeMap`), like `serde_json` without
//!   `preserve_order`, so the output is deterministic.

use std::collections::BTreeMap;
use std::fmt;

/// Same recursion limit as `serde_json`.
const MAX_DEPTH: usize = 128;

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Null,
    Bool(bool),
    /// Validated JSON number literal, kept verbatim.
    Num(String),
    Str(String),
    Arr(Vec<Value>),
    Obj(BTreeMap<String, Value>),
}

#[derive(Debug, PartialEq, Eq)]
pub struct Error(pub String);

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

fn err<T>(msg: impl Into<String>) -> Result<T, Error> {
    Err(Error(msg.into()))
}

impl Value {
    pub fn is_null(&self) -> bool {
        matches!(self, Value::Null)
    }

    pub fn from_i64(n: i64) -> Value {
        Value::Num(n.to_string())
    }

    /// `Some` only for integer literals that fit an `i64`.
    pub fn as_i64(&self) -> Option<i64> {
        match self {
            // `i64::from_str` accepts exactly `-?[0-9]+` (plus a leading `+`,
            // which the number grammar never lets through), so "1.0" / "1e2"
            // are rejected, like serde's integer deserializer does.
            Value::Num(s) => s.parse::<i64>().ok(),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(b) => Some(*b),
            _ => None,
        }
    }

    /// Parse a complete JSON text. Trailing non-whitespace is an error.
    pub fn parse(text: &str) -> Result<Value, Error> {
        let mut p = Parser { s: text.as_bytes(), i: 0 };
        let v = p.value(0)?;
        p.ws();
        if p.i != p.s.len() {
            return err(format!("trailing characters at byte {}", p.i));
        }
        Ok(v)
    }

    /// Like [`Value::parse`], for bytes that must be UTF-8.
    pub fn parse_slice(bytes: &[u8]) -> Result<Value, Error> {
        match std::str::from_utf8(bytes) {
            Ok(t) => Value::parse(t),
            Err(_) => err("invalid UTF-8"),
        }
    }

    /// Compact serialization.
    pub fn write_to(&self, out: &mut String) {
        match self {
            Value::Null => out.push_str("null"),
            Value::Bool(true) => out.push_str("true"),
            Value::Bool(false) => out.push_str("false"),
            Value::Num(n) => out.push_str(n),
            Value::Str(s) => write_str(out, s),
            Value::Arr(a) => {
                out.push('[');
                for (i, v) in a.iter().enumerate() {
                    if i > 0 {
                        out.push(',');
                    }
                    v.write_to(out);
                }
                out.push(']');
            }
            Value::Obj(o) => {
                out.push('{');
                for (i, (k, v)) in o.iter().enumerate() {
                    if i > 0 {
                        out.push(',');
                    }
                    write_str(out, k);
                    out.push(':');
                    v.write_to(out);
                }
                out.push('}');
            }
        }
    }

    pub fn to_json(&self) -> String {
        let mut s = String::new();
        self.write_to(&mut s);
        s
    }
}

/// Escape like `serde_json`: `"` `\` and C0 controls only; everything else
/// (including non-ASCII and `/`) is emitted as-is.
pub fn write_str(out: &mut String, s: &str) {
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => {
                out.push_str("\\u00");
                out.push(char::from(b"0123456789abcdef"[(c as usize) >> 4]));
                out.push(char::from(b"0123456789abcdef"[(c as usize) & 15]));
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
}

impl Parser<'_> {
    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    fn ws(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t' | b'\n' | b'\r')) {
            self.i += 1;
        }
    }

    fn eat(&mut self, c: u8) -> Result<(), Error> {
        if self.peek() == Some(c) {
            self.i += 1;
            Ok(())
        } else {
            err(format!("expected '{}' at byte {}", c as char, self.i))
        }
    }

    fn lit(&mut self, word: &str, v: Value) -> Result<Value, Error> {
        if self.s[self.i..].starts_with(word.as_bytes()) {
            self.i += word.len();
            Ok(v)
        } else {
            err(format!("invalid literal at byte {}", self.i))
        }
    }

    fn value(&mut self, depth: usize) -> Result<Value, Error> {
        if depth > MAX_DEPTH {
            return err("recursion limit exceeded");
        }
        self.ws();
        match self.peek() {
            None => err("unexpected end of input"),
            Some(b'n') => self.lit("null", Value::Null),
            Some(b't') => self.lit("true", Value::Bool(true)),
            Some(b'f') => self.lit("false", Value::Bool(false)),
            Some(b'"') => Ok(Value::Str(self.string()?)),
            Some(b'[') => {
                self.i += 1;
                let mut a = Vec::new();
                self.ws();
                if self.peek() == Some(b']') {
                    self.i += 1;
                    return Ok(Value::Arr(a));
                }
                loop {
                    a.push(self.value(depth + 1)?);
                    self.ws();
                    match self.peek() {
                        Some(b',') => self.i += 1,
                        Some(b']') => {
                            self.i += 1;
                            return Ok(Value::Arr(a));
                        }
                        _ => return err(format!("expected ',' or ']' at byte {}", self.i)),
                    }
                }
            }
            Some(b'{') => {
                self.i += 1;
                let mut o = BTreeMap::new();
                self.ws();
                if self.peek() == Some(b'}') {
                    self.i += 1;
                    return Ok(Value::Obj(o));
                }
                loop {
                    self.ws();
                    if self.peek() != Some(b'"') {
                        return err(format!("expected string key at byte {}", self.i));
                    }
                    let k = self.string()?;
                    self.ws();
                    self.eat(b':')?;
                    let v = self.value(depth + 1)?;
                    if o.insert(k, v).is_some() {
                        return err("duplicate object key");
                    }
                    self.ws();
                    match self.peek() {
                        Some(b',') => self.i += 1,
                        Some(b'}') => {
                            self.i += 1;
                            return Ok(Value::Obj(o));
                        }
                        _ => return err(format!("expected ',' or '}}' at byte {}", self.i)),
                    }
                }
            }
            Some(b'-' | b'0'..=b'9') => self.number(),
            Some(_) => err(format!("unexpected character at byte {}", self.i)),
        }
    }

    fn digits(&mut self) -> usize {
        let start = self.i;
        while matches!(self.peek(), Some(b'0'..=b'9')) {
            self.i += 1;
        }
        self.i - start
    }

    /// `-? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?`
    fn number(&mut self) -> Result<Value, Error> {
        let start = self.i;
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        match self.peek() {
            Some(b'0') => self.i += 1,
            Some(b'1'..=b'9') => {
                self.digits();
            }
            _ => return err(format!("invalid number at byte {start}")),
        }
        if self.peek() == Some(b'.') {
            self.i += 1;
            if self.digits() == 0 {
                return err(format!("invalid number at byte {start}"));
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.i += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if self.digits() == 0 {
                return err(format!("invalid number at byte {start}"));
            }
        }
        // The slice is pure ASCII by construction.
        let text = std::str::from_utf8(&self.s[start..self.i]).map_err(|_| Error("invalid number".into()))?;
        Ok(Value::Num(text.to_owned()))
    }

    fn hex4(&mut self) -> Result<u32, Error> {
        let mut v = 0u32;
        for _ in 0..4 {
            let d = match self.peek() {
                Some(c @ b'0'..=b'9') => c - b'0',
                Some(c @ b'a'..=b'f') => c - b'a' + 10,
                Some(c @ b'A'..=b'F') => c - b'A' + 10,
                _ => return err(format!("invalid \\u escape at byte {}", self.i)),
            };
            v = v * 16 + u32::from(d);
            self.i += 1;
        }
        Ok(v)
    }

    fn string(&mut self) -> Result<String, Error> {
        self.eat(b'"')?;
        let mut out = String::new();
        loop {
            // Copy a run of plain bytes in one go. Input came from a &str and
            // the run boundaries are ASCII, so every run is valid UTF-8.
            let run_start = self.i;
            while let Some(c) = self.peek() {
                if c == b'"' || c == b'\\' || c < 0x20 {
                    break;
                }
                self.i += 1;
            }
            if self.i > run_start {
                match std::str::from_utf8(&self.s[run_start..self.i]) {
                    Ok(t) => out.push_str(t),
                    Err(_) => return err("invalid UTF-8"),
                }
            }
            match self.peek() {
                None => return err("unterminated string"),
                Some(b'"') => {
                    self.i += 1;
                    return Ok(out);
                }
                Some(b'\\') => {
                    self.i += 1;
                    let c = self.peek().ok_or_else(|| Error("unterminated escape".into()))?;
                    self.i += 1;
                    match c {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{08}'),
                        b'f' => out.push('\u{0c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let hi = self.hex4()?;
                            let cp = match hi {
                                0xD800..=0xDBFF => {
                                    if !self.s[self.i..].starts_with(b"\\u") {
                                        return err("lone leading surrogate");
                                    }
                                    self.i += 2;
                                    let lo = self.hex4()?;
                                    if !(0xDC00..=0xDFFF).contains(&lo) {
                                        return err("invalid surrogate pair");
                                    }
                                    0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00)
                                }
                                0xDC00..=0xDFFF => return err("lone trailing surrogate"),
                                _ => hi,
                            };
                            match char::from_u32(cp) {
                                Some(ch) => out.push(ch),
                                None => return err("invalid code point"),
                            }
                        }
                        _ => return err(format!("invalid escape at byte {}", self.i - 1)),
                    }
                }
                Some(_) => return err(format!("control character in string at byte {}", self.i)),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(s: &str) -> Value {
        Value::parse(s).unwrap()
    }

    #[test]
    fn scalars_and_containers() {
        assert_eq!(p(" null "), Value::Null);
        assert_eq!(p("true"), Value::Bool(true));
        assert_eq!(p("[1, \"a\", [], {}]").to_json(), "[1,\"a\",[],{}]");
        assert_eq!(p("{\"b\":1,\"a\":2}").to_json(), "{\"a\":2,\"b\":1}");
    }

    #[test]
    fn numbers_roundtrip_verbatim_and_validate() {
        for ok in ["0", "-0", "7", "-12", "1.5", "1e9", "1E+9", "-2.5e-3", "12345678901234567890"] {
            assert_eq!(p(ok).to_json(), ok);
        }
        for bad in ["01", "-", "+1", "1.", ".5", "1e", "1e+", "0x1", "--1", "NaN", "Infinity"] {
            assert!(Value::parse(bad).is_err(), "{bad} must be rejected");
        }
        assert_eq!(p("9223372036854775807").as_i64(), Some(i64::MAX));
        assert_eq!(p("9223372036854775808").as_i64(), None);
        assert_eq!(p("-9223372036854775808").as_i64(), Some(i64::MIN));
        assert_eq!(p("1.0").as_i64(), None);
        assert_eq!(p("1e2").as_i64(), None);
    }

    #[test]
    fn string_escapes() {
        assert_eq!(p(r#""a\"b\\c\/d\b\f\n\r\t""#).as_str(), Some("a\"b\\c/d\u{8}\u{c}\n\r\t"));
        assert_eq!(p(r#""\u00e4\u0041""#).as_str(), Some("äA"));
        assert_eq!(p(r#""\ud83d\udc4d""#).as_str(), Some("👍"));
        assert_eq!(p("\"👍 ünï\"").as_str(), Some("👍 ünï"));
        let mut out = String::new();
        write_str(&mut out, "q\" \\ \n\t\u{1}\u{1f} / ä 👍 \u{7f}");
        assert_eq!(out, "\"q\\\" \\\\ \\n\\t\\u0001\\u001f / ä 👍 \u{7f}\"");
        assert_eq!(Value::parse(&out).unwrap().as_str(), Some("q\" \\ \n\t\u{1}\u{1f} / ä 👍 \u{7f}"));
    }

    #[test]
    fn rejects_malformed_input() {
        for bad in [
            "", " ", "nul", "tru", "[", "[1,]", "[1 2]", "{", "{,}", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{a:1}",
            "{'a':1}", "\"abc", "\"a\nb\"", "\"\\x\"", "\"\\u12\"", "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"",
            "1 2", "null x", "[1]]", "//c\n1", "{\"a\":1,\"a\":2}",
        ] {
            assert!(Value::parse(bad).is_err(), "{bad:?} must be rejected");
        }
        assert!(Value::parse_slice(&[b'"', 0xff, b'"']).is_err());
    }

    #[test]
    fn depth_limit_prevents_stack_overflow() {
        let ok = format!("{}{}", "[".repeat(MAX_DEPTH), "]".repeat(MAX_DEPTH));
        assert!(Value::parse(&ok).is_ok());
        let deep = format!("{}{}", "[".repeat(100_000), "]".repeat(100_000));
        assert!(Value::parse(&deep).is_err());
        let deep_obj = format!("{}1{}", "{\"a\":".repeat(100_000), "}".repeat(100_000));
        assert!(Value::parse(&deep_obj).is_err());
    }
}
