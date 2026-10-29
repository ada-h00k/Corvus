// signal-cli's jsonRpc framing: one JSON object per line on stdin/stdout.

use serde_json::{json, Map, Value};

pub const METHOD_NOT_FOUND: i64 = -32601;
pub const INVALID_PARAMS: i64 = -32602;
/// signal-cli reports failed commands with this code.
pub const COMMAND_FAILED: i64 = -1;

#[derive(Debug)]
pub struct RpcError {
    pub code: i64,
    pub message: String,
}

impl RpcError {
    pub fn failed(message: impl Into<String>) -> Self {
        Self { code: COMMAND_FAILED, message: message.into() }
    }

    pub fn invalid(message: impl Into<String>) -> Self {
        Self { code: INVALID_PARAMS, message: message.into() }
    }

    pub fn unsupported(method: &str) -> Self {
        Self { code: METHOD_NOT_FOUND, message: format!("{method} is not supported by corvus-signal yet") }
    }
}

impl<E: std::fmt::Display> From<E> for RpcError {
    fn from(e: E) -> Self {
        Self::failed(e.to_string())
    }
}

pub type RpcResult = Result<Value, RpcError>;

#[derive(Debug)]
pub struct Request {
    pub id: Option<Value>,
    pub method: String,
    pub params: Params,
}

/// Parses one input line. Err carries the response for a malformed line (null id, like JSON-RPC wants).
pub fn parse_request(line: &str) -> Result<Request, Value> {
    let value: Value = serde_json::from_str(line)
        .map_err(|e| error_response(Value::Null, -32700, &format!("parse error: {e}")))?;
    let Value::Object(mut obj) = value else {
        return Err(error_response(Value::Null, -32600, "invalid request"));
    };
    let id = obj.remove("id");
    let Some(Value::String(method)) = obj.remove("method") else {
        return Err(error_response(id.unwrap_or(Value::Null), -32600, "invalid request: no method"));
    };
    let params = match obj.remove("params") {
        None | Some(Value::Null) => Map::new(),
        Some(Value::Object(m)) => m,
        Some(_) => return Err(error_response(id.unwrap_or(Value::Null), INVALID_PARAMS, "params must be an object")),
    };
    Ok(Request { id, method, params: Params(params) })
}

pub fn response(id: Value, result: RpcResult) -> Value {
    match result {
        Ok(result) => json!({"jsonrpc": "2.0", "id": id, "result": result}),
        Err(e) => error_response(id, e.code, &e.message),
    }
}

fn error_response(id: Value, code: i64, message: &str) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "error": {"code": code, "message": message}})
}

pub fn notification(method: &str, params: Value) -> Value {
    json!({"jsonrpc": "2.0", "method": method, "params": params})
}

/// Typed access to request params. signal-cli accepts a single string where it takes a list
/// (and the reverse for single-value options), so both shapes are accepted here too.
#[derive(Debug, Default)]
pub struct Params(pub Map<String, Value>);

impl Params {
    pub fn str(&self, key: &str) -> Option<&str> {
        match self.0.get(key)? {
            Value::String(s) => Some(s),
            Value::Array(a) => a.first()?.as_str(),
            _ => None,
        }
    }

    pub fn required_str(&self, key: &str) -> Result<&str, RpcError> {
        self.str(key).ok_or_else(|| RpcError::invalid(format!("missing parameter '{key}'")))
    }

    pub fn strings(&self, key: &str) -> Vec<String> {
        match self.0.get(key) {
            Some(Value::String(s)) => vec![s.clone()],
            Some(Value::Array(a)) => a.iter().filter_map(|v| v.as_str().map(str::to_owned)).collect(),
            _ => Vec::new(),
        }
    }

    pub fn bool(&self, key: &str) -> bool {
        match self.0.get(key) {
            Some(Value::Bool(b)) => *b,
            Some(Value::String(s)) => s == "true",
            _ => false,
        }
    }

    pub fn u64(&self, key: &str) -> Option<u64> {
        match self.0.get(key)? {
            Value::Number(n) => n.as_u64(),
            Value::String(s) => s.parse().ok(),
            _ => None,
        }
    }

    pub fn required_u64(&self, key: &str) -> Result<u64, RpcError> {
        self.u64(key).ok_or_else(|| RpcError::invalid(format!("missing parameter '{key}'")))
    }

    pub fn u64s(&self, key: &str) -> Vec<u64> {
        match self.0.get(key) {
            Some(Value::Array(a)) => a
                .iter()
                .filter_map(|v| v.as_u64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
                .collect(),
            Some(_) => self.u64(key).into_iter().collect(),
            None => Vec::new(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_request() {
        let r = parse_request(r#"{"jsonrpc":"2.0","method":"send","id":3,"params":{"recipient":["+49"],"message":"hi"}}"#).unwrap();
        assert_eq!(r.method, "send");
        assert_eq!(r.id, Some(json!(3)));
        assert_eq!(r.params.strings("recipient"), ["+49"]);
        assert_eq!(r.params.str("message"), Some("hi"));
    }

    #[test]
    fn params_are_optional() {
        let r = parse_request(r#"{"jsonrpc":"2.0","method":"listGroups","id":1}"#).unwrap();
        assert!(r.params.0.is_empty());
    }

    #[test]
    fn malformed_lines_get_error_responses() {
        let e = parse_request("not json").unwrap_err();
        assert_eq!(e["error"]["code"], -32700);
        let e = parse_request(r#"{"id":4}"#).unwrap_err();
        assert_eq!(e["id"], 4);
        assert_eq!(e["error"]["code"], -32600);
    }

    #[test]
    fn scalar_and_list_shapes() {
        let p = Params(serde_json::from_str(r#"{"a":"x","b":["y","z"],"t":["5",6],"n":"7"}"#).unwrap());
        assert_eq!(p.strings("a"), ["x"]);
        assert_eq!(p.str("b"), Some("y"));
        assert_eq!(p.u64s("t"), [5, 6]);
        assert_eq!(p.u64("n"), Some(7));
    }

    #[test]
    fn responses() {
        assert_eq!(response(json!(1), Ok(json!({}))), json!({"jsonrpc":"2.0","id":1,"result":{}}));
        let e = response(json!(2), Err(RpcError::unsupported("updateGroup")));
        assert_eq!(e["error"]["code"], METHOD_NOT_FOUND);
        assert!(e["error"]["message"].as_str().unwrap().contains("updateGroup"));
    }
}
