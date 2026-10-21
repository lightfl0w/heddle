use crate::graph::{Graph, Node};
use crate::proc::{self, ProcResult};
use std::collections::VecDeque;
use std::sync::{Condvar, Mutex};

pub struct SchedOpts<'a> {
    pub graph: &'a Graph,
    pub cwd: Option<&'a str>,
    pub logdir: &'a str,
    pub jobs: usize,
    pub retry: usize,
    pub keep_going: bool,
    pub active: Option<&'a [u8]>,
    pub env: &'a [(String, String)],
}

struct Shared {
    ready: VecDeque<usize>,
    done: Vec<(usize, ProcResult)>,
    shutdown: bool,
}

fn cmd_to_str(n: &Node) -> String {
    n.argv.join(" ")
}

fn dump_log(logdir: &str, nd: usize) {
    let logpath = format!("{}/{}.log", logdir, nd);
    let text = match std::fs::read(&logpath) {
        Ok(b) => String::from_utf8_lossy(&b).into_owned(),
        Err(_) => {
            eprintln!("  (no log at {})", logpath);
            return;
        }
    };
    let mut shown = 0usize;
    let mut more = 0usize;
    for line in text.split_inclusive('\n') {
        if shown < 60 {
            eprint!("  | ");
            eprint!("{}", line);
            if !line.ends_with('\n') {
                eprintln!();
            }
            shown += 1;
        } else {
            more += 1;
        }
    }
    if shown > 0 && !text.ends_with('\n') && shown <= 60 {
        eprintln!();
    }
    if shown + more > 60 {
        eprintln!("  | ... {} more line(s)", more);
    }
    if shown == 0 && text.is_empty() {
        eprintln!("  (log empty: {})", logpath);
    }
}

fn report_failure(g: &Graph, logdir: &str, nd: usize, r: &ProcResult) {
    eprintln!("\nerror: build step failed");
    if r.signaled {
        eprintln!("  signal: {}", r.signal);
    } else {
        eprintln!("  exit:   {}", r.exit_code);
    }
    eprintln!("  command: {}", cmd_to_str(&g.nodes[nd]));
    eprintln!("  output:");
    dump_log(logdir, nd);
    eprintln!();
}

fn block_descendants(g: &Graph, start: usize, blocked: &mut [u8], active: &[u8]) -> usize {
    let mut count = 0usize;
    let mut stack = vec![start];
    blocked[start] = 1;
    while let Some(cur) = stack.pop() {
        for k in 0..g.nodes[cur].rdeps.len() {
            let d = g.nodes[cur].rdeps[k];
            if blocked[d] != 0 || active[d] == 0 {
                continue;
            }
            stack.push(d);
            blocked[d] = 1;
            count += 1;
        }
    }
    count
}

pub fn run(o: &SchedOpts) -> i32 {
    let n = o.graph.nodes.len();
    let owned_active;
    let active: &[u8] = match o.active {
        Some(a) => a,
        None => {
            owned_active = vec![1u8; if n == 0 { 1 } else { n }];
            &owned_active
        }
    };

    let total = (0..n).filter(|i| active[*i] != 0).count();
    if total == 0 {
        return 0;
    }

    let shared = Mutex::new(Shared {
        ready: VecDeque::new(),
        done: Vec::new(),
        shutdown: false,
    });
    let cv_ready = Condvar::new();
    let cv_done = Condvar::new();

    let mut tries = vec![0usize; if n == 0 { 1 } else { n }];
    let mut done = vec![0u8; if n == 0 { 1 } else { n }];
    let mut blocked = vec![0u8; if n == 0 { 1 } else { n }];
    let mut indeg = vec![0usize; if n == 0 { 1 } else { n }];
    for i in 0..n {
        done[i] = if active[i] == 0 { 1 } else { 0 };
        indeg[i] = o.graph.nodes[i].indeg;
    }
    for i in 0..n {
        if active[i] != 0 {
            continue;
        }
        for k in 0..o.graph.nodes[i].rdeps.len() {
            let d = o.graph.nodes[i].rdeps[k];
            if active[d] != 0 && indeg[d] > 0 {
                indeg[d] -= 1;
            }
        }
    }
    {
        let mut s = shared.lock().unwrap();
        for i in 0..n {
            if active[i] != 0 && indeg[i] == 0 {
                s.ready.push_back(i);
            }
        }
    }

    let mut nthreads = if o.jobs < 1 { 1 } else { o.jobs };
    if nthreads > total {
        nthreads = total;
    }

    let mut remaining = total;
    let mut failed = false;

    std::thread::scope(|scope| {
        for _ in 0..nthreads {
            let shared = &shared;
            let cv_ready = &cv_ready;
            let cv_done = &cv_done;
            let g = o.graph;
            let logdir = o.logdir;
            let cwd = o.cwd;
            let env = o.env;
            scope.spawn(move || loop {
                let node = {
                    let mut s = shared.lock().unwrap();
                    while s.ready.is_empty() && !s.shutdown {
                        s = cv_ready.wait(s).unwrap();
                    }
                    if s.ready.is_empty() && s.shutdown {
                        break;
                    }
                    s.ready.pop_front().unwrap()
                };
                let logpath = format!("{}/{}.log", logdir, node);
                let r = proc::run(&g.nodes[node].argv, cwd, &logpath, env).unwrap_or(ProcResult {
                    exit_code: -1,
                    signaled: false,
                    signal: 0,
                });
                let mut s = shared.lock().unwrap();
                s.done.push((node, r));
                cv_done.notify_one();
            });
        }

        while remaining > 0 {
            let local: Vec<(usize, ProcResult)> = {
                let mut s = shared.lock().unwrap();
                while s.done.is_empty() {
                    s = cv_done.wait(s).unwrap();
                }
                std::mem::take(&mut s.done)
            };
            for (nd, r) in local {
                if blocked[nd] != 0 {
                    continue;
                }
                if r.exit_code == 0 && !r.signaled {
                    done[nd] = 1;
                    remaining -= 1;
                    let mut s = shared.lock().unwrap();
                    for k in 0..o.graph.nodes[nd].rdeps.len() {
                        let d = o.graph.nodes[nd].rdeps[k];
                        if blocked[d] != 0 || done[d] != 0 || active[d] == 0 {
                            continue;
                        }
                        if indeg[d] > 0 {
                            indeg[d] -= 1;
                        }
                        if indeg[d] == 0 {
                            s.ready.push_back(d);
                        }
                    }
                    cv_ready.notify_all();
                } else {
                    tries[nd] += 1;
                    if tries[nd] <= o.retry {
                        {
                            let mut s = shared.lock().unwrap();
                            s.ready.push_back(nd);
                            cv_ready.notify_one();
                        }
                        eprintln!("[retry {}/{}] node {}", tries[nd], o.retry, nd);
                        continue;
                    }
                    report_failure(o.graph, o.logdir, nd, &r);
                    failed = true;
                    remaining -= 1;
                    remaining -= block_descendants(o.graph, nd, &mut blocked, active);
                }
            }
            if failed && !o.keep_going {
                for i in 0..n {
                    if active[i] != 0 && done[i] == 0 && blocked[i] == 0 {
                        blocked[i] = 1;
                    }
                }
                break;
            }
        }

        {
            let mut s = shared.lock().unwrap();
            s.ready.clear();
            s.shutdown = true;
            cv_ready.notify_all();
        }
    });

    if failed {
        1
    } else {
        0
    }
}
