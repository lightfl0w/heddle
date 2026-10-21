pub fn version_string() -> String {
    format!(
        "{} ({}, {})",
        option_env!("BUILD_VERSION").unwrap_or("0.0.0-dev"),
        option_env!("BUILD_TARGET").unwrap_or("unknown"),
        option_env!("BUILD_COMMIT").unwrap_or("unknown")
    )
}
