use serde_json::Value;
use std::collections::{HashMap, HashSet};

#[derive(Default, Clone)]
pub struct Json {
    items: Vec<(String, String)>,
    map: HashMap<String, usize>,
    nodes: HashSet<String>,
}

fn flatten(v: &Value, path: &str, out: &mut Json) {
    match v {
        Value::Object(m) => {
            if !path.is_empty() {
                out.nodes.insert(path.to_string());
            }
            for (k, val) in m {
                let sub = if path.is_empty() {
                    k.clone()
                } else {
                    format!("{}.{}", path, k)
                };
                flatten(val, &sub, out);
            }
        }
        Value::Array(a) => {
            if !path.is_empty() {
                out.nodes.insert(path.to_string());
            }
            for (i, val) in a.iter().enumerate() {
                flatten(val, &format!("{}[{}]", path, i), out);
            }
        }
        Value::String(s) => out.put(path, s),
        other => out.put(path, &other.to_string()),
    }
}

impl Json {
    fn put(&mut self, key: &str, val: &str) {
        if let Some(&i) = self.map.get(key) {
            self.items[i].1 = val.to_string();
            return;
        }
        self.items.push((key.to_string(), val.to_string()));
        self.map.insert(key.to_string(), self.items.len() - 1);
    }

    pub fn parse(path: &str) -> Result<Json, String> {
        let bytes = crate::sys::read_bytes(path).ok_or_else(|| format!("cannot read {}", path))?;
        let v: Value = serde_json::from_slice(&bytes)
            .map_err(|e| format!("{}: {}", path, e))?;
        if !v.is_object() {
            return Err(format!("{}: not a json object", path));
        }
        let mut j = Json::default();
        flatten(&v, "", &mut j);
        Ok(j)
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.map.get(key).map(|&i| self.items[i].1.as_str())
    }

    pub fn has_node(&self, path: &str) -> bool {
        self.map.contains_key(path) || self.nodes.contains(path)
    }

    pub fn arr_len(&self, path: &str) -> usize {
        let mut n = 0usize;
        while self.has_node(&format!("{}[{}]", path, n)) {
            n += 1;
        }
        n
    }
}
