use toml::Value;

#[derive(Default, Clone)]
pub struct TomlTable {
    pub name: String,
    pub items: Vec<(String, String)>,
}

#[derive(Default, Clone)]
pub struct Toml {
    pub tables: Vec<TomlTable>,
}

fn table_find(t: &Toml, name: &str) -> Option<usize> {
    t.tables.iter().position(|x| x.name == name)
}

fn table_add(t: &mut Toml, name: &str) -> usize {
    if let Some(i) = table_find(t, name) {
        return i;
    }
    t.tables.push(TomlTable {
        name: name.to_string(),
        items: Vec::new(),
    });
    t.tables.len() - 1
}

fn table_put(tab: &mut TomlTable, key: &str, val: &str) {
    if let Some(e) = tab.items.iter_mut().find(|(k, _)| k == key) {
        e.1 = val.to_string();
        return;
    }
    tab.items.push((key.to_string(), val.to_string()));
}

fn scalar_text(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        other => other.to_string(),
    }
}

fn walk(v: &Value, name: &str, t: &mut Toml) {
    let m = match v {
        Value::Table(m) => m,
        _ => return,
    };
    let cur = table_add(t, name);
    for (k, val) in m {
        match val {
            Value::Table(_) | Value::Array(_) => {}
            _ => {
                table_put(&mut t.tables[cur], k, &scalar_text(val));
            }
        }
    }
    for (k, val) in m {
        let sub = if name.is_empty() {
            k.clone()
        } else {
            format!("{}.{}", name, k)
        };
        match val {
            Value::Table(_) => walk(val, &sub, t),
            Value::Array(a) => {
                if a.iter().all(|x| matches!(x, Value::Table(_))) && !a.is_empty() {
                    for item in a {
                        walk(item, &sub, t);
                    }
                } else {
                    let cur = table_add(t, name);
                    for (i, item) in a.iter().enumerate() {
                        table_put(
                            &mut t.tables[cur],
                            &format!("{}[{}]", k, i),
                            &scalar_text(item),
                        );
                    }
                }
            }
            _ => {}
        }
    }
}

impl Toml {
    pub fn new() -> Toml {
        let mut t = Toml { tables: Vec::new() };
        table_add(&mut t, "");
        t
    }

    pub fn parse(path: &str) -> Result<Toml, String> {
        let text = crate::sys::read_to_string(path)
            .ok_or_else(|| format!("cannot read {}", path))?;
        let root: Value = toml::from_str(&text)
            .map_err(|e| format!("{}: {}", path, e))?;
        let mut t = Toml::new();
        walk(&root, "", &mut t);
        Ok(t)
    }

    pub fn str_of(&self, section: &str, key: &str) -> Option<&str> {
        let i = table_find(self, section)?;
        self.tables[i]
            .items
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v.as_str())
    }

    pub fn arr(&self, section: &str, key: &str, index: usize) -> Option<&str> {
        let norm = format!("{}[{}]", key, index);
        self.str_of(section, &norm)
    }

    pub fn bool_of(&self, section: &str, key: &str, fb: bool) -> bool {
        match self.str_of(section, key) {
            Some("true") => true,
            Some("false") => false,
            _ => fb,
        }
    }

    pub fn sections(&self, prefix: &str) -> Vec<String> {
        let mut out = Vec::new();
        for t in &self.tables {
            if !t.name.starts_with(prefix) {
                continue;
            }
            let rest = &t.name[prefix.len()..];
            if rest.contains('.') {
                continue;
            }
            out.push(rest.to_string());
        }
        out
    }
}

pub fn parse_atoi(s: &str) -> i64 {
    crate::sys::atoi(s)
}
