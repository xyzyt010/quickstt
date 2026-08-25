//! Error types for QuickSTT Core

use thiserror::Error;

/// Main error type for QuickSTT Core operations
#[derive(Error, Debug)]
pub enum QuickSttError {
    #[error("STT service error: {0}")]
    SttServiceError(String),

    #[error("STT service not running")]
    ServiceNotRunning,

    #[error("STT service executable not found: {0}")]
    ServiceNotFound(String),

    #[error("IPC communication error: {0}")]
    IpcError(String),

    #[error("Settings error: {0}")]
    SettingsError(String),

    #[error("Registry error: {0}")]
    RegistryError(String),

    #[error("Model error: {0}")]
    ModelError(String),

    #[error("Model not found: {0}")]
    ModelNotFound(String),

    #[error("Model not installed: {0}")]
    ModelNotInstalled(String),

    #[error("Cloud STT error: {0}")]
    CloudSttError(String),

    #[error("Cloud provider error ({provider}): {message}")]
    CloudProviderError { provider: String, message: String },

    #[error("Network error: {0}")]
    NetworkError(String),

    #[error("IO error: {0}")]
    IoError(#[from] std::io::Error),

    #[error("Configuration error: {0}")]
    ConfigError(String),

    #[error("Win32 error: {0}")]
    Win32Error(String),

    #[error("Platform error: {0}")]
    PlatformError(String),

    #[error("Manager error ({manager}): {message}")]
    ManagerError { manager: String, message: String },

    #[error("Smart Life error: {0}")]
    SmartLifeError(String),

    #[error("Android TV error: {0}")]
    AndroidTvError(String),

    #[error("Home Assistant error: {0}")]
    HomeAssistantError(String),

    #[error("AHK bridge error: {0}")]
    AhkBridgeError(String),

    #[error("Optional service error: {0}")]
    OptionalServiceError(String),

    #[error("Command processing error: {0}")]
    CommandError(String),

    #[error("Setup not completed")]
    SetupNotCompleted,

    #[error("Unknown error: {0}")]
    Unknown(String),
}

/// Result type alias for QuickSTT Core
pub type QuickSttResult<T> = std::result::Result<T, QuickSttError>;

impl QuickSttError {
    /// Create a service error
    pub fn service_error(msg: impl Into<String>) -> Self {
        QuickSttError::SttServiceError(msg.into())
    }

    /// Create an IPC error
    pub fn ipc_error(msg: impl Into<String>) -> Self {
        QuickSttError::IpcError(msg.into())
    }

    /// Create a model error
    pub fn model_error(msg: impl Into<String>) -> Self {
        QuickSttError::ModelError(msg.into())
    }

    /// Create a cloud STT error
    pub fn cloud_error(msg: impl Into<String>) -> Self {
        QuickSttError::CloudSttError(msg.into())
    }

    /// Create a Win32 error
    pub fn win32_error(msg: impl Into<String>) -> Self {
        QuickSttError::Win32Error(msg.into())
    }

    pub fn platform_error(msg: impl Into<String>) -> Self {
        QuickSttError::PlatformError(msg.into())
    }
}

#[cfg(target_os = "windows")]
impl From<windows::core::Error> for QuickSttError {
    fn from(err: windows::core::Error) -> Self {
        QuickSttError::Win32Error(format!("Win32 error: {}", err))
    }
}
