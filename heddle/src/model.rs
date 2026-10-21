#[derive(Clone, Copy, PartialEq, Eq, Default)]
pub enum TargetType {
    #[default]
    Exe,
    StaticLib,
    SharedLib,
    Raw,
    Custom,
}

impl TargetType {
    pub fn as_str(&self) -> &'static str {
        match self {
            TargetType::Exe => "exe",
            TargetType::StaticLib => "staticlib",
            TargetType::SharedLib => "sharedlib",
            TargetType::Raw => "raw",
            TargetType::Custom => "custom",
        }
    }

    pub fn parse(s: &str) -> Option<TargetType> {
        match s {
            "exe" | "executable" | "bin" => Some(TargetType::Exe),
            "staticlib" | "lib" => Some(TargetType::StaticLib),
            "sharedlib" | "dylib" | "so" => Some(TargetType::SharedLib),
            "raw" => Some(TargetType::Raw),
            "custom" => Some(TargetType::Custom),
            _ => None,
        }
    }
}

#[derive(Clone, Default)]
pub struct Target {
    pub name: String,
    pub dir: String,
    pub kind: TargetType,
    pub src: Vec<String>,
    pub inc: Vec<String>,
    pub deps: Vec<String>,
    pub cflags: Vec<String>,
    pub ldflags: Vec<String>,
    pub cmd: Option<String>,
    pub out: Option<String>,
    pub ldscript: Option<String>,
    pub entry: Option<String>,
    pub format: Option<String>,
    pub start_group: bool,
    pub whole_archive: bool,
}

#[derive(Clone, Default)]
pub struct Toolchain {
    pub name: String,
    pub family: String,
    pub cc: String,
    pub cxx: String,
    pub ar: String,
    pub ld: String,
    pub cflags: Vec<String>,
    pub ldflags: Vec<String>,
    pub objext: String,
    pub binext: String,
    pub libext: String,
    pub dllpre: String,
    pub dllext: String,
    pub soflag: String,
    pub as_: String,
    pub platform: String,
    pub env: Vec<String>,
}

#[derive(Clone, Copy, PartialEq, Eq, Default)]
pub enum PkgKind {
    #[default]
    Toolchain,
    Library,
}

#[derive(Clone, Copy, PartialEq, Eq, Default)]
pub enum PkgSource {
    #[default]
    Auto,
    Source,
    Binary,
    PkgConfig,
}

#[derive(Clone, Default)]
pub struct PkgSpec {
    pub name: String,
    pub version: String,
    pub target: String,
    pub kind: PkgKind,
    pub source: PkgSource,
    pub hash: String,
    pub store_path: String,
    pub pc_name: String,
    pub cflags: Vec<String>,
    pub ldflags: Vec<String>,
    pub recipe: bool,
}

#[derive(Clone, Default)]
pub struct TargetProfile {
    pub arch: String,
    pub abi: String,
    pub float_k: String,
    pub prefix: String,
    pub sysroot: String,
    pub cpu: String,
    pub fpu: String,
}

#[derive(Clone, Default)]
pub struct PkgManifest {
    pub root: String,
    pub store: String,
    pub lock_path: String,
    pub manifest: String,
    pub toolchain_prefix: Option<String>,
    pub sysroot: Option<String>,
    pub tools: Vec<PkgSpec>,
    pub deps: Vec<PkgSpec>,
    pub target: TargetProfile,
}

#[derive(Clone, Default)]
pub struct Project {
    pub root: String,
    pub build_dir: String,
    pub toolchain_name: String,
    pub tc: Toolchain,
    pub pkg: PkgManifest,
    pub target_prefix: Option<String>,
    pub target_sysroot: Option<String>,
    pub tc_based: Option<String>,
    pub tc_cc: Option<String>,
    pub tc_family: Option<String>,
    pub targets: Vec<Target>,
}

#[derive(Clone, Default)]
pub struct StarTargetDef {
    pub name: String,
    pub kind: Option<String>,
    pub src: Vec<String>,
    pub inc: Vec<String>,
    pub deps: Vec<String>,
    pub pkgdeps: Vec<String>,
    pub platform: Option<String>,
    pub cflags: Vec<String>,
    pub ldflags: Vec<String>,
    pub ldscript: Option<String>,
    pub entry: Option<String>,
    pub out: Option<String>,
    pub start_group: bool,
    pub whole_archive: bool,
    pub i_bin: Vec<String>,
    pub i_lib: Vec<String>,
    pub i_include: Vec<String>,
    pub i_share: Vec<String>,
    pub i_etc: Vec<String>,
    pub i_rootfs: Option<String>,
}

#[derive(Clone, Default)]
pub struct StarPlatform {
    pub name: String,
    pub arch: String,
    pub abi: String,
    pub flt: String,
    pub sysroot: String,
    pub toolchain: String,
}

#[derive(Clone, Default)]
pub struct StarCfg {
    pub root: String,
    pub file: String,
    pub project_name: Option<String>,
    pub build_dir: Option<String>,
    pub toolchain: Option<String>,
    pub store: Option<String>,
    pub arch: String,
    pub abi: String,
    pub flt: String,
    pub sysroot: String,
    pub platforms: Vec<StarPlatform>,
    pub tc_name: Vec<String>,
    pub tc_cc: Vec<String>,
    pub tc_based: Vec<String>,
    pub dep_name: Vec<String>,
    pub dep_ver: Vec<String>,
    pub dep_src: Vec<String>,
    pub targets: Vec<StarTargetDef>,
}

#[derive(Clone, Default)]
pub struct RecipePkg {
    pub name: String,
    pub version: String,
}

#[derive(Clone, Default)]
pub struct RecipeSource {
    pub url: String,
    pub tag: String,
    pub path: String,
}

#[derive(Clone, Default)]
pub struct RecipeBuild {
    pub patterns: Vec<String>,
    pub files: Vec<String>,
    pub include_dirs: Vec<String>,
    pub defines: Vec<String>,
    pub cflags: Vec<String>,
    pub kind: Option<String>,
}

#[derive(Clone, Default)]
pub struct Recipe {
    pub pkg: RecipePkg,
    pub source: RecipeSource,
    pub build: RecipeBuild,
    pub dir: String,
}

#[derive(Clone, Default)]
pub struct InstallRule {
    pub name: String,
    pub bin: Vec<String>,
    pub lib: Vec<String>,
    pub include: Vec<String>,
    pub share: Vec<String>,
    pub etc: Vec<String>,
    pub sysroot_lib: Vec<String>,
    pub sysroot_include: Vec<String>,
    pub rootfs: Option<String>,
}

#[derive(Clone, Default)]
pub struct InstallItem {
    pub src: String,
    pub dst: String,
}

#[derive(Clone, Default)]
pub struct InstallPlan {
    pub items: Vec<InstallItem>,
}

#[derive(Clone, Default)]
pub struct HeddleOpts {
    pub root: String,
    pub target: Option<String>,
    pub toolchain: Option<String>,
    pub cache: Option<String>,
    pub remote: Option<String>,
    pub jobs: usize,
    pub verbose: bool,
    pub no_cache: bool,
    pub offline: bool,
    pub registry: Option<String>,
    pub prefix: Option<String>,
    pub destdir: Option<String>,
    pub dry_run: bool,
    pub compile_db: Option<String>,
    pub interval_ms: i64,
    pub run_args: Vec<String>,
}

