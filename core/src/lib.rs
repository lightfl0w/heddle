pub mod build;
pub mod cas;
pub mod deps;
pub mod graph;
pub mod hash;
pub mod incremental;
pub mod proc;
pub mod scheduler;
pub mod sys;
pub mod version;

pub type Res<T> = Result<T, String>;
