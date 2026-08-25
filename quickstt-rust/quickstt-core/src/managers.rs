//! Manager trait definitions and implementations

use anyhow::Result;
use async_trait::async_trait;

/// Base trait for all managers
#[async_trait]
pub trait Manager: Send + Sync {
    /// Initialize the manager (load settings, connect, etc.)
    async fn initialize(&mut self) -> Result<()>;

    /// Shutdown the manager (disconnect, cleanup)
    async fn shutdown(&mut self) -> Result<()>;

    /// Check if the manager is ready
    fn is_ready(&self) -> bool;
}

/// Trait for managers that can be started/stopped
#[async_trait]
pub trait Startable: Manager {
    /// Start the manager
    async fn start(&mut self) -> Result<()>;

    /// Stop the manager
    async fn stop(&mut self) -> Result<()>;

    /// Check if the manager is running
    fn is_running(&self) -> bool;
}

/// Trait for managers that handle settings
#[async_trait]
pub trait SettingsAware: Manager {
    /// Load settings from registry
    async fn load_settings(&mut self) -> Result<()>;

    /// Save current settings to registry
    async fn save_settings(&self) -> Result<()>;
}

/// Trait for managers that provide status information
pub trait StatusProvider: Manager {
    /// Get current status as a string
    fn status(&self) -> String;

    /// Get detailed status information
    fn detailed_status(&self) -> serde_json::Value;
}

// Re-export manager implementations as they're added
// pub mod cloud_stt;
// pub mod local_model;
// pub mod local_frontend;
// pub mod smart_life;
// pub mod android_tv;
// pub mod home_assistant;
// pub mod ahk_bridge;
// pub mod optional_service;
