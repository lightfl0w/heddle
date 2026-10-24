use crate::graph::{Graph, Node};
use crate::hash::{hash_read_file, hash_u64, HashDb, HASH_SEED};
use crate::sys;

pub const NODE_REC: usize = 32;

#[derive(Clone, Copy, Default)]
pub struct NodeState {
    pub cmd_hash: u64,
    pub in_hash: u64,
    pub out_hash: u64,
    pub valid: bool,
}

pub struct IncrDb {
    pub n: usize,
    pub states: Vec<NodeState>,
    pub files: HashDb,
}

impl IncrDb {
    pub fn new(n: usize) -> IncrDb {
        IncrDb {
            n,
            states: vec![NodeState::default(); if n == 0 { 1 } else { n }],
            files: HashDb::new(),
        }
    }

    pub fn load(&mut self, path: &str) -> bool {
        let data = match sys::read_bytes(path) {
            Some(d) => d,
            None => return false,
        };
        if data.len() < 8 {
            return false;
        }
        let magic = i32::from_le_bytes([data[0], data[1], data[2], data[3]]);
        let n = i32::from_le_bytes([data[4], data[5], data[6], data[7]]);
        if magic != 0x4c4f4f4d || n < 0 || n as usize != self.n {
            return false;
        }
        if data.len() < 8 + self.n * NODE_REC {
            return false;
        }
        for i in 0..self.n {
            let o = 8 + i * NODE_REC;
            self.states[i].cmd_hash = u64::from_le_bytes(data[o..o + 8].try_into().unwrap());
            self.states[i].in_hash = u64::from_le_bytes(data[o + 8..o + 16].try_into().unwrap());
            self.states[i].out_hash = u64::from_le_bytes(data[o + 16..o + 24].try_into().unwrap());
            let v = i32::from_le_bytes(data[o + 24..o + 28].try_into().unwrap());
            self.states[i].valid = v != 0;
        }
        true
    }

    pub fn save(&self, path: &str) -> bool {
        let mut out: Vec<u8> = Vec::with_capacity(8 + self.n * NODE_REC);
        out.extend_from_slice(&0x4c4f4f4di32.to_le_bytes());
        out.extend_from_slice(&(self.n as i32).to_le_bytes());
        for st in self.states.iter().take(self.n) {
            out.extend_from_slice(&st.cmd_hash.to_le_bytes());
            out.extend_from_slice(&st.in_hash.to_le_bytes());
            out.extend_from_slice(&st.out_hash.to_le_bytes());
            out.extend_from_slice(&(if st.valid { 1i32 } else { 0i32 }).to_le_bytes());
            out.extend_from_slice(&[0u8; 4]);
        }
        sys::write_bytes_if_changed(path, &out)
    }
}

pub fn node_input_hash(nd: &Node, files: &mut HashDb) -> u64 {
    let mut h = nd.cmd_hash;
    for i in &nd.ins {
        h = hash_u64(h, hash_read_file(Some(files), i));
    }
    for d in &nd.dyns {
        h = hash_u64(h, hash_read_file(Some(files), d));
    }
    h
}

pub fn node_output_hash(nd: &Node, files: &mut HashDb) -> u64 {
    let mut h = HASH_SEED;
    for o in &nd.outs {
        h = hash_u64(h, hash_read_file(Some(files), o));
    }
    h
}

fn node_dirty(db: &mut IncrDb, g: &Graph, i: usize) -> bool {
    let st = db.states[i];
    if !st.valid {
        return true;
    }
    let nd = &g.nodes[i];
    if st.cmd_hash != nd.cmd_hash {
        return true;
    }
    if st.in_hash != node_input_hash(nd, &mut db.files) {
        return true;
    }
    if st.out_hash != node_output_hash(nd, &mut db.files) {
        return true;
    }
    false
}

pub fn plan(db: &mut IncrDb, g: &Graph, active: &mut [u8]) -> usize {
    for (i, a) in active.iter_mut().enumerate() {
        *a = if i < g.nodes.len() && node_dirty(db, g, i) {
            1
        } else {
            0
        };
    }
    let mut count = 0;
    for i in 0..db.n {
        if active[i] == 0 {
            continue;
        }
        count += 1;
        for &d in &g.nodes[i].rdeps {
            active[d] = 1;
        }
    }
    count
}

pub fn record(db: &mut IncrDb, g: &Graph, node: usize) {
    let nd = &g.nodes[node];
    let ch = nd.cmd_hash;
    let ih = node_input_hash(nd, &mut db.files);
    let oh = node_output_hash(nd, &mut db.files);
    db.states[node].cmd_hash = ch;
    db.states[node].in_hash = ih;
    db.states[node].out_hash = oh;
    db.states[node].valid = true;
}

pub fn commit(db: &mut IncrDb, g: &Graph, active: &[u8]) {
    for (i, &a) in active.iter().enumerate().take(db.n) {
        if a == 0 {
            continue;
        }
        record(db, g, i);
    }
}
