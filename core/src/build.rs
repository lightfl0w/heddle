use crate::cas::{AcOut, ActionCache, Cas};
use crate::deps;
use crate::graph::{self, Graph};
use crate::hash::{hash_read_file, hash_u64, HashDb, HASH_FNV_OFFSET};
use crate::incremental::{self, IncrDb};
use crate::scheduler::{self, SchedOpts};
use crate::sys;
use std::collections::HashMap;

pub struct BuildOpts {
    pub graph_file: String,
    pub cwd: Option<String>,
    pub logdir: String,
    pub jobs: usize,
    pub retry: usize,
    pub keep_going: bool,
    pub cache_dir: Option<String>,
    pub remote: Option<String>,
    pub no_cache: bool,
    pub env: Vec<(String, String)>,
}

pub struct BuildEngine {
    opts: BuildOpts,
    graph: Graph,
    db: IncrDb,
    active: Vec<u8>,
    cas: Cas,
    ac: ActionCache,
    pub ran: usize,
    pub cached: usize,
    state_path: String,
    hash_path: String,
    deps_path: String,
    loaded: bool,
    tools: HashMap<String, u64>,
}

impl Drop for BuildEngine {
    fn drop(&mut self) {
        self.ac.save();
    }
}

impl BuildEngine {
    pub fn open(o: BuildOpts) -> Result<BuildEngine, String> {
        let text = match sys::read_to_string(&o.graph_file) {
            Some(t) => t,
            None => return Err(format!("cannot read {}", o.graph_file)),
        };
        let g = graph::parse(&text)?;
        sys::mkpath(&o.logdir);
        let n = g.nodes.len();
        let state_path = format!("{}/loom.db", o.logdir);
        let hash_path = format!("{}/loom.hash", o.logdir);
        let deps_path = format!("{}/loom.deps", o.logdir);
        let cache_root = o.cache_dir.clone().unwrap_or_else(|| o.logdir.clone());
        let cas = Cas::open(&cache_root, o.remote.as_deref());
        let mut ac = ActionCache::new(&cache_root);
        ac.load();
        Ok(BuildEngine {
            opts: o,
            db: IncrDb::new(n),
            graph: g,
            active: vec![0u8; if n == 0 { 1 } else { n }],
            cas,
            ac,
            ran: 0,
            cached: 0,
            state_path,
            hash_path,
            deps_path,
            loaded: false,
            tools: HashMap::new(),
        })
    }

    fn db_load(&mut self) {
        if self.loaded {
            return;
        }
        self.db.files = HashDb::new();
        self.db.files.load(&self.hash_path);
        self.db.load(&self.state_path);
        deps::load(&mut self.graph, &self.deps_path);
        self.loaded = true;
    }

    fn tool_resolve(&self, name: &str) -> u64 {
        if name.is_empty() {
            return 0;
        }
        if name.contains('/') {
            return hash_read_file(None, name);
        }
        let path = match std::env::var_os("PATH") {
            Some(p) => p,
            None => return 0,
        };
        for dir in std::env::split_paths(&path) {
            let cand = dir.join(name);
            let h = hash_read_file(None, &cand.to_string_lossy());
            if h != 0 {
                return h;
            }
        }
        0
    }

    fn tool_hash(&mut self, name: &str) -> u64 {
        if let Some(h) = self.tools.get(name) {
            return *h;
        }
        let h = self.tool_resolve(name);
        self.tools.insert(name.to_string(), h);
        h
    }

    fn node_last_input(&mut self, i: usize) -> u64 {
        let nd = &self.graph.nodes[i];
        let mut k = nd.cmd_hash;
        let ins = nd.ins.clone();
        let outs = nd.outs.clone();
        for ip in &ins {
            if outs.iter().any(|o| o == ip) {
                continue;
            }
            k = hash_u64(k, hash_read_file(Some(&mut self.db.files), ip));
        }
        k
    }

    fn deps_key(&mut self, i: usize) -> u64 {
        let mut k = HASH_FNV_OFFSET;
        let dyns = self.graph.nodes[i].dyns.clone();
        for d in &dyns {
            k = hash_u64(k, hash_read_file(Some(&mut self.db.files), d));
        }
        k
    }

    fn cache_key(&mut self, i: usize) -> u64 {
        let intrinsic = self.node_last_input(i);
        let dynk = self.deps_key(i);
        let key = hash_u64(intrinsic, dynk);
        let tool = self.graph.nodes[i].argv.first().cloned().unwrap_or_default();
        let th = self.tool_hash(&tool);
        hash_u64(key, th)
    }

    fn deps_ready(&self, node: usize) -> bool {
        let nd = &self.graph.nodes[node];
        for d in &nd.deps {
            if self.active[*d] != 0 {
                return false;
            }
        }
        true
    }

    fn cache_restore(&mut self, dirty: &mut usize) {
        for i in 0..self.db.n {
            if self.active[i] == 0 {
                continue;
            }
            if i >= self.graph.nodes.len() {
                continue;
            }
            if self.graph.nodes[i].outs.is_empty() {
                continue;
            }
            if !self.deps_ready(i) {
                continue;
            }
            let key = self.cache_key(i);
            let ent = match self.ac.find(key) {
                Some(e) => e,
                None => continue,
            };
            let outs: Vec<AcOut> = self
                .ac
                .items[ent]
                .outs
                .iter()
                .map(|o| AcOut {
                    hash: o.hash,
                    mode: o.mode,
                })
                .collect();
            if !outs.iter().all(|o| self.cas.has(o.hash)) {
                continue;
            }
            let targets = self.graph.nodes[i].outs.clone();
            let mut restored = true;
            for (k, o) in outs.iter().enumerate() {
                let dst = match targets.get(k) {
                    Some(t) => t.clone(),
                    None => {
                        restored = false;
                        break;
                    }
                };
                if !self.cas.get_file(o.hash, &dst) {
                    restored = false;
                    break;
                }
            }
            if !restored {
                continue;
            }
            for (k, o) in outs.iter().enumerate() {
                if let Some(t) = targets.get(k) {
                    if sys::stat(t).is_some() {
                        sys::chmod(t, o.mode);
                    }
                }
            }
            incremental::record(&mut self.db, &self.graph, i);
            self.active[i] = 0;
            self.cached += 1;
            *dirty -= 1;
        }
    }

    fn cache_store(&mut self) {
        for i in 0..self.db.n {
            if self.active[i] == 0 || i >= self.graph.nodes.len() {
                continue;
            }
            if self.graph.nodes[i].outs.is_empty() {
                continue;
            }
            let key = self.cache_key(i);
            let outs_names = self.graph.nodes[i].outs.clone();
            let mut outs: Vec<AcOut> = Vec::new();
            let mut ok = true;
            for o in &outs_names {
                let st = match sys::stat(o) {
                    Some(s) => s,
                    None => {
                        ok = false;
                        break;
                    }
                };
                let h = hash_read_file(Some(&mut self.db.files), o);
                if h == 0 || !self.cas.put_hashed(o, h) {
                    ok = false;
                    break;
                }
                outs.push(AcOut {
                    hash: h,
                    mode: st.mode & 0o7777,
                });
            }
            if ok {
                self.ac.put(key, &outs);
            }
        }
    }

    fn save_state(&mut self) {
        incremental::commit(&mut self.db, &self.graph, &self.active);
        self.db.files.save(&self.hash_path);
        self.db.save(&self.state_path);
        deps::save(&self.graph, &self.deps_path);
    }

    pub fn run(&mut self) -> i32 {
        self.db_load();
        deps::scan(&mut self.graph, &mut self.db.files);
        if let Err(e) = graph::finalize(&mut self.graph) {
            eprintln!("error: {}", e);
            return 1;
        }

        let mut active = std::mem::take(&mut self.active);
        let mut dirty = incremental::plan(&mut self.db, &self.graph, &mut active);
        self.active = active;
        if dirty == 0 {
            return 0;
        }

        self.ran = 0;
        self.cached = 0;
        if !self.opts.no_cache {
            self.cache_restore(&mut dirty);
        }
        if dirty == 0 {
            self.save_state();
            return 0;
        }

        self.ran = dirty;
        let so = SchedOpts {
            graph: &self.graph,
            cwd: self.opts.cwd.as_deref(),
            logdir: &self.opts.logdir,
            jobs: self.opts.jobs,
            retry: self.opts.retry,
            keep_going: self.opts.keep_going,
            active: Some(&self.active),
            env: &self.opts.env,
        };
        let rc = scheduler::run(&so);
        if rc == 0 {
            if !self.opts.no_cache {
                self.cache_store();
            }
            self.save_state();
        }
        rc
    }
}
