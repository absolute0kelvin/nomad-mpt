/**
 * @file nomad_mpt.h
 * @brief MonadDB MPT (Merkle Patricia Trie) - Pure C FFI Interface
 * 
 * This header provides a stable C API for MonadDB, suitable for:
 * - Go (via CGO)
 * - Java (via JNI or Panama FFM)
 * - C# (via P/Invoke)
 * - Python (via ctypes/cffi)
 * - Any language with C FFI support
 * 
 * For Rust, use the nomad-mpt-sys crate which provides a more idiomatic
 * interface via cxx bridge.
 * 
 * Thread Safety:
 * - NomadDb: NOT thread-safe for writes, safe for concurrent reads
 * - NomadFifo: Thread-safe (lock-free FIFO queues)
 * 
 * Memory Management:
 * - All nomad_*_create functions return owning pointers
 * - Caller must call corresponding nomad_*_destroy to free
 * - Returned byte arrays (nomad_bytes_t) must be freed with nomad_bytes_free
 */

#ifndef NOMAD_MPT_H
#define NOMAD_MPT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Version Info
 * ============================================================ */

#define NOMAD_MPT_VERSION_MAJOR 0
#define NOMAD_MPT_VERSION_MINOR 1
#define NOMAD_MPT_VERSION_PATCH 0

/** Get version string (e.g., "0.1.0") */
const char* nomad_version(void);

/* ============================================================
 * Opaque Types
 * ============================================================ */

/** Database handle */
typedef struct NomadDb NomadDb;

/** Node handle (reference-counted) */
typedef struct NomadNode NomadNode;

/** Async FIFO handle */
typedef struct NomadFifo NomadFifo;

/* ============================================================
 * Result Types
 * ============================================================ */

/** Error codes */
typedef enum {
    NOMAD_OK = 0,
    NOMAD_ERR_NULL_POINTER = 1,
    NOMAD_ERR_INVALID_ARGUMENT = 2,
    NOMAD_ERR_NOT_FOUND = 3,
    NOMAD_ERR_IO = 4,
    NOMAD_ERR_VERSION_OUT_OF_RANGE = 5,
    NOMAD_ERR_NOT_SUPPORTED = 6,
    NOMAD_ERR_OUT_OF_MEMORY = 7,
    NOMAD_ERR_INTERNAL = 255,
} NomadError;

/** Byte array with length (caller must free with nomad_bytes_free) */
typedef struct {
    uint8_t* data;
    size_t len;
} NomadBytes;

/** Free a NomadBytes structure */
void nomad_bytes_free(NomadBytes* bytes);

/* ============================================================
 * Database Lifecycle
 * ============================================================ */

/**
 * Open an in-memory database
 * 
 * @param[out] db_out Pointer to receive the database handle
 * @return NOMAD_OK on success, error code otherwise
 */
NomadError nomad_db_open_memory(NomadDb** db_out);

/**
 * Open a disk-based database (read-write mode)
 * 
 * @param path Database directory path
 * @param create If true, create database if it doesn't exist
 * @param history_length Number of historical versions to retain (0 = default)
 * @param[out] db_out Pointer to receive the database handle
 * @return NOMAD_OK on success, error code otherwise
 */
NomadError nomad_db_open_disk(
    const char* path,
    bool create,
    uint64_t history_length,
    NomadDb** db_out
);

/**
 * Close and free a database handle
 * 
 * @param db Database handle (NULL is safe)
 */
void nomad_db_close(NomadDb* db);

/**
 * Check if database is using disk storage
 */
bool nomad_db_is_on_disk(const NomadDb* db);

/**
 * Check if database is read-only
 */
bool nomad_db_is_read_only(const NomadDb* db);

/* ============================================================
 * Synchronous Read Operations
 * ============================================================ */

/**
 * Find a key in the database (disk mode only)
 * 
 * Note: This function only works in disk mode. For in-memory databases,
 * use nomad_db_find_from_root() instead.
 * 
 * @param db Database handle
 * @param key Key bytes
 * @param key_len Key length
 * @param version Version number to query
 * @param[out] node_out Pointer to receive the node handle (NULL if not found)
 * @return NOMAD_OK on success, NOMAD_ERR_NOT_SUPPORTED for memory mode
 */
NomadError nomad_db_find(
    const NomadDb* db,
    const uint8_t* key,
    size_t key_len,
    uint64_t version,
    NomadNode** node_out
);

/**
 * Find a key starting from a root node (works in both memory and disk mode)
 * 
 * @param db Database handle
 * @param root Root node to search from (can be from upsert result)
 * @param key Key bytes
 * @param key_len Key length
 * @param version Version number
 * @param[out] node_out Pointer to receive the node handle (NULL if not found)
 * @return NOMAD_OK on success (even if not found), error code otherwise
 */
NomadError nomad_db_find_from_root(
    const NomadDb* db,
    const NomadNode* root,
    const uint8_t* key,
    size_t key_len,
    uint64_t version,
    NomadNode** node_out
);

/**
 * Get the value from a node
 * 
 * @param node Node handle
 * @param[out] value_out Pointer to receive the value (caller must free)
 * @return NOMAD_OK on success, NOMAD_ERR_NOT_FOUND if no value
 */
NomadError nomad_node_get_value(const NomadNode* node, NomadBytes* value_out);

/**
 * Check if a node has a value
 */
bool nomad_node_has_value(const NomadNode* node);

/**
 * Get the Merkle hash of a node (32 bytes)
 * 
 * @param node Node handle
 * @param[out] hash_out Buffer to receive the hash (must be at least 32 bytes)
 * @return NOMAD_OK on success
 */
NomadError nomad_node_get_hash(const NomadNode* node, uint8_t* hash_out);

/**
 * Clone a node handle (increment reference count)
 */
NomadNode* nomad_node_clone(const NomadNode* node);

/**
 * Free a node handle
 */
void nomad_node_free(NomadNode* node);

/* ============================================================
 * Synchronous Write Operations
 * ============================================================ */

/** Update operation type */
typedef enum {
    NOMAD_UPDATE_PUT = 0,
    NOMAD_UPDATE_DELETE = 1,
} NomadUpdateType;

/** Single update entry */
typedef struct {
    NomadUpdateType type;
    const uint8_t* key;
    size_t key_len;
    const uint8_t* value;      /* NULL for delete */
    size_t value_len;
} NomadUpdate;

/**
 * Apply updates to the database
 * 
 * @param db Database handle
 * @param root Current root node (NULL for empty tree)
 * @param updates Array of updates
 * @param updates_len Number of updates
 * @param version Target version number
 * @param[out] new_root_out Pointer to receive the new root node
 * @return NOMAD_OK on success
 */
NomadError nomad_db_upsert(
    NomadDb* db,
    const NomadNode* root,
    const NomadUpdate* updates,
    size_t updates_len,
    uint64_t version,
    NomadNode** new_root_out
);

/* ============================================================
 * Version Management
 * ============================================================ */

/** Get the latest version number */
uint64_t nomad_db_get_latest_version(const NomadDb* db);

/** Get the earliest available version number */
uint64_t nomad_db_get_earliest_version(const NomadDb* db);

/** Get the history retention length */
uint64_t nomad_db_get_history_length(const NomadDb* db);

/** Load the root node for a specific version */
NomadError nomad_db_load_root(const NomadDb* db, uint64_t version, NomadNode** root_out);

/** Check if a version is valid */
bool nomad_db_version_is_valid(const NomadDb* db, uint64_t version);

/* ============================================================
 * Async FIFO Operations (EXPERIMENTAL)
 * 
 * NOTE: The async FIFO API is currently experimental and NOT fully
 * implemented in the C API. For high-throughput scenarios, consider
 * using the Rust bindings which have a complete async implementation.
 * 
 * Current status:
 * - nomad_fifo_create: Implemented (creates placeholder)
 * - nomad_fifo_start/stop: Stub (no-op)
 * - nomad_fifo_submit_*: Stub (no-op)
 * - nomad_fifo_poll*: Stub (always returns false)
 * ============================================================ */

/** Request type for async operations */
typedef enum {
    NOMAD_REQ_FIND_VALUE = 1,
    NOMAD_REQ_FIND_NODE = 2,
    NOMAD_REQ_TRAVERSE = 3,
} NomadRequestType;

/** Result status for async operations */
typedef enum {
    NOMAD_STATUS_OK = 0,
    NOMAD_STATUS_NOT_FOUND = 1,
    NOMAD_STATUS_ERROR = 2,
    NOMAD_STATUS_TRAVERSE_MORE = 3,
    NOMAD_STATUS_TRAVERSE_END = 4,
} NomadResultStatus;

/** Completion result from async operations */
typedef struct {
    uint64_t user_data_lo;
    uint64_t user_data_hi;
    NomadResultStatus status;
    uint32_t value_len;
    uint8_t value[256];         /* Inline value for small results */
    uint8_t merkle_hash[32];
} NomadCompletion;

/**
 * Create an async FIFO for a database
 * 
 * @param db Database handle
 * @param[out] fifo_out Pointer to receive the FIFO handle
 * @return NOMAD_OK on success
 */
NomadError nomad_fifo_create(NomadDb* db, NomadFifo** fifo_out);

/**
 * Start the FIFO worker threads
 * 
 * @param fifo FIFO handle
 * @param num_workers Number of worker fibers (recommend 4-8)
 */
void nomad_fifo_start(NomadFifo* fifo, size_t num_workers);

/**
 * Stop the FIFO workers
 */
void nomad_fifo_stop(NomadFifo* fifo);

/**
 * Destroy the FIFO (also stops workers)
 */
void nomad_fifo_destroy(NomadFifo* fifo);

/**
 * Submit a find request (non-blocking)
 * 
 * @param fifo FIFO handle
 * @param key Key bytes
 * @param key_len Key length
 * @param version Version to query
 * @param user_data_lo User data (low 64 bits), returned in completion
 * @param user_data_hi User data (high 64 bits), returned in completion
 */
void nomad_fifo_submit_find(
    NomadFifo* fifo,
    const uint8_t* key,
    size_t key_len,
    uint64_t version,
    uint64_t user_data_lo,
    uint64_t user_data_hi
);

/**
 * Submit a traverse request (non-blocking)
 * 
 * @param fifo FIFO handle
 * @param prefix Key prefix bytes
 * @param prefix_len Prefix length
 * @param version Version to query
 * @param limit Maximum number of results (0 = default 4096)
 * @param user_data_lo User data (low 64 bits)
 * @param user_data_hi User data (high 64 bits)
 */
void nomad_fifo_submit_traverse(
    NomadFifo* fifo,
    const uint8_t* prefix,
    size_t prefix_len,
    uint64_t version,
    uint32_t limit,
    uint64_t user_data_lo,
    uint64_t user_data_hi
);

/**
 * Poll for a completion (non-blocking)
 * 
 * @param fifo FIFO handle
 * @param[out] completion_out Pointer to receive the completion
 * @return true if a completion was available, false otherwise
 */
bool nomad_fifo_poll(NomadFifo* fifo, NomadCompletion* completion_out);

/**
 * Poll for traverse results (non-blocking)
 * 
 * @param fifo FIFO handle
 * @param[out] completion_out Pointer to receive the completion
 * @return true if a result was available, false otherwise
 */
bool nomad_fifo_poll_traverse(NomadFifo* fifo, NomadCompletion* completion_out);

/**
 * Poll for a large value (>256 bytes)
 * 
 * @param fifo FIFO handle
 * @param[out] user_data_lo User data from the original request
 * @param[out] user_data_hi User data from the original request
 * @param[out] data_out Pointer to receive the data (caller must free)
 * @return true if a large value was available, false otherwise
 */
bool nomad_fifo_poll_large_value(
    NomadFifo* fifo,
    uint64_t* user_data_lo,
    uint64_t* user_data_hi,
    NomadBytes* data_out
);

#ifdef __cplusplus
}
#endif

#endif /* NOMAD_MPT_H */

