/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2013-2017 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef Engine_Cache_h
#define Engine_Cache_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>
#include <sstream> // stringstream
#include <fstream>
#include <functional>
#include <list>
#include <set>
#include <cstddef>
#include <utility>
#include <algorithm> // min, max

#include "Global/GlobalDefines.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/uuid/uuid.hpp>
#endif

#include "Engine/CacheEntryBase.h"

#include "Engine/EngineFwd.h"

// Each 8 bit tile will have pow(2, tileSizePo2) pixels in each dimension.
// 16 bit tiles will have one side halved
// 32 bit tiles will have both dimension halved (so tile size for 32bit is actually pow(2, tileSizePo2-1)
//


#define NATRON_TILE_SIZE_X_8_BIT 128
#define NATRON_TILE_SIZE_Y_8_BIT 128

#define NATRON_TILE_SIZE_X_16_BIT NATRON_TILE_SIZE_X_8_BIT
#define NATRON_TILE_SIZE_Y_16_BIT (NATRON_TILE_SIZE_Y_8_BIT / 2)

#define NATRON_TILE_SIZE_X_32_BIT (NATRON_TILE_SIZE_X_8_BIT / 2)
#define NATRON_TILE_SIZE_Y_32_BIT (NATRON_TILE_SIZE_Y_8_BIT / 2)

#define NATRON_TILE_SIZE_BYTES (NATRON_TILE_SIZE_X_8_BIT * NATRON_TILE_SIZE_Y_8_BIT)


// The name of the directory containing all buckets on disk
#define NATRON_CACHE_DIRECTORY_NAME "Cache"


// When defined, the cache will never be persistent.
//
// Non-persistent cache:
// ---------------------
// In this mode, all objects in the cache (classes deriving CacheEntryBase)
// are shared across threads within the process: When looking-up an entry with
// Cache::get, the entry might already exist, in which case it can be retrieved by calling
// CacheEntryLocker::getProcessLocalEntry.
// It is very important that in this mode derived classes of CacheEntryBase be thread-safe.
//
// Persistent cache:
// -----------------
//
// In this mode, all objects in the cache are not actually exposed to the CacheEntryBase:
// whenever reading an entry from the cache in the Cache::get function, internally a copy
// is made to the local CacheEntryBase object by using the CacheEntryBase::fromMemorySegment
// function.
// Similarly, when inserting the entry in the cache, it is copied using the CacheEntryBase::toMemorySegment
// In this mode, the CacheEntryBase in itself does not have to be thread-safe: the cache itself handles
// the thread-safety and ensures that the entry is created only once.
// It is important to remember that in this mode, the CacheEntryBase you pass to Cache::get is local to your call, nobody
// else knows about this object.
// Also in order to be persistent, all data structures passed in toMemorySegment/fromMemorySegment must be interprocess compliant
// (i.e: they must use allocators to the memory segment manager passed in parameter.)

// Interprocess robust:
//  -------------------
// If defined the cache can handle multiple processes accessing to the same cache concurrently, however
// the cache may not be placed in a network drive.
// If not defined, the cache supports only a single process writing/reading from the cache concurrently, other processes will resort
// in a process-local cache.
#define NATRON_CACHE_INTERPROCESS_ROBUST

NATRON_NAMESPACE_ENTER;


struct CacheReportInfo
{
    int nEntries;
    std::size_t nBytes;

    CacheReportInfo()
    : nEntries(0)
    , nBytes(0)
    {

    }
};

template <bool persistent>
struct CacheBucket;

/**
 * @brief Small RAII style class used to lock an entry corresponding to a hash key to ensure
 * only a single thread can work on it at once.
 * Mainly this is to avoid multiple threads from computing the same image at once.
 **/
class CacheEntryLockerBase
{

public:

    enum CacheEntryStatusEnum
    {
        // The entry is cached and may be retrieved from 
        eCacheEntryStatusCached,

        // The entry was not cached and must be computed by this thread
        // When computed it is expected that this thread calls CacheEntryLocker::insert()
        // when done.
        eCacheEntryStatusMustCompute,

        // The entry was not cached but another thread is already computing it
        // This thread should wait
        eCacheEntryStatusComputationPending
    };

    CacheEntryLockerBase()
    {
        
    }


    virtual ~CacheEntryLockerBase()
    {

    }

    /**
     * @brief Returns whether the associated cache entry is persistent or not
     **/
    virtual bool isPersistent() const = 0;

    /**
     * @brief Returns the cache entry status
     **/
    virtual CacheEntryStatusEnum getStatus() const = 0;

    /**
     * @brief If the entry status was eCacheEntryStatusMustCompute, it should be called
     * to insert the results into the cache. This will also set the status to eCacheEntryStatusCached
     * and threads waiting for this entry will be woken up and should have it available.
     **/
    virtual void insertInCache() = 0;

    /**
     * @brief If the status was eCacheEntryStatusComputationPending, this waits for the results
     * to be available by another thread that called insertInCache().
     * If results are ready when woken up
     * the status will become eCacheEntryStatusCached and the entry passed to the constructor is ready.
     * If the status can still not be found in the cache and no other threads is computing it, the status
     * will become eCacheEntryStatusMustCompute and it is expected that this thread computes the entry in turn.
     *
     * @param timeout If set to 0, the process will wait forever until the entry becomes available.
     * Otherwise this process will wait up to "timeout" milliseconds for the pending entry. After that, if it still
     * is not available, it will takeover the entry and return a status of eCacheEntryStatusMustCompute.
     **/
    virtual CacheEntryStatusEnum waitForPendingEntry(std::size_t timeout = 0) = 0;


    /**
     * @brief Get the entry that was originally passed to the ctor.
     **/
    virtual CacheEntryBasePtr getProcessLocalEntry() const = 0;

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    /**
     * @brief Returns the process that is in charge of computing the entry UUID.
     * This can be used to determine if the entry was abandonned.
     **/
    virtual boost::uuids::uuid getComputeProcessUUID() const = 0;
#endif

    static void sleep_milliseconds(std::size_t amountMS);

};

template <bool persistent>
struct CacheEntryLockerPrivate;
template <bool persistent>
class CacheEntryLocker : public CacheEntryLockerBase
{
    // For create
    friend class Cache<persistent>;

    CacheEntryLocker(const boost::shared_ptr<Cache<persistent> >& cache, const CacheEntryBasePtr& entry);

    static boost::shared_ptr<CacheEntryLocker<persistent> > create(const boost::shared_ptr<Cache<persistent> >& cache, const CacheEntryBasePtr& entry);

public:

    virtual ~CacheEntryLocker();

    /**
     * @brief Returns whether the associated cache entry is persistent or not
     **/
    virtual bool isPersistent() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Returns the cache entry status
     **/
    virtual CacheEntryStatusEnum getStatus() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief If the entry status was eCacheEntryStatusMustCompute, it should be called
     * to insert the results into the cache. This will also set the status to eCacheEntryStatusCached
     * and threads waiting for this entry will be woken up and should have it available.
     **/
    virtual void insertInCache() OVERRIDE FINAL;

    /**
     * @brief If the status was eCacheEntryStatusComputationPending, this waits for the results
     * to be available by another thread that called insertInCache().
     * If results are ready when woken up
     * the status will become eCacheEntryStatusCached and the entry passed to the constructor is ready.
     * If the status can still not be found in the cache and no other threads is computing it, the status
     * will become eCacheEntryStatusMustCompute and it is expected that this thread computes the entry in turn.
     *
     * @param timeout If set to 0, the process will wait forever until the entry becomes available.
     * Otherwise this process will wait up to "timeout" milliseconds for the pending entry. After that, if it still
     * is not available, it will takeover the entry and return a status of eCacheEntryStatusMustCompute.
     **/
    virtual CacheEntryStatusEnum waitForPendingEntry(std::size_t timeout = 0) OVERRIDE FINAL WARN_UNUSED_RETURN;

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    virtual boost::uuids::uuid getComputeProcessUUID() const OVERRIDE FINAL WARN_UNUSED_RETURN;
#endif

    /**
     * @brief Get the entry that was originally passed to the ctor.
     **/
    virtual CacheEntryBasePtr getProcessLocalEntry() const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:

    boost::scoped_ptr<CacheEntryLockerPrivate<persistent> > _imp;
};

template <bool persistent>
struct CachePrivate;

class CacheBase
{


public:

    CacheBase()
    {

    }

    virtual ~CacheBase()
    {

    }

    /**
     * @brief Check if the given file exists on disk
     **/
    static bool fileExists(const std::string& filename);

    /**
     * @brief Return a number 0 <= N <= 255 from the 2 first hexadecimal digits (8-bit) of the hash
     **/
    static int getBucketCacheBucketIndex(U64 hash);


    /**
     * @brief Returns the tile size (of one dimension) in pixels for the given bitdepth/
     **/
    static void getTileSizePx(ImageBitDepthEnum bitdepth, int *tx, int *ty);

    /**
     * @brief Returns whether the cache is persistent or not
     **/
    virtual bool isPersistent() const = 0;

    /**
     * @brief Returns the absolute path to the cache directory
     **/
    virtual std::string getCacheDirectoryPath() const = 0;


    /**
     * @brief Set the maximum cache size available for the given storage.
     * Note that if shrinking, this will clear from the cache exceeding entries.
     **/
    virtual void setMaximumCacheSize(std::size_t size) = 0;

    /**
     * @brief Returns the maximum cache size for the given storage.
     **/
    virtual std::size_t getMaximumCacheSize() const = 0;

    /**
     * @breif Returns the actual size taken in memory for the given storagE.
     **/
    virtual std::size_t getCurrentSize() const = 0;

    /**
     * @brief Look-up the cache for the given entry's key.
     * The entry is assumed to have its key set.
     * This function return a cache entry locker object that itself indicate
     * the CacheEntryLocker:CacheEntryStatusEnum of the entry. Depending
     * on the status the thread can continue
     * if results were found in cache, or compute the entry and then call CacheEntryLocker::insertInCache()
     * or just wait for another thread that is already computing the entry with
     * CacheEntryLocker::waitForPendingEntry.
     * NB: if the cache is not persistent the entry pointer may be modified
     **/
    virtual CacheEntryLockerBasePtr get(const CacheEntryBasePtr& entry) const = 0;

    /**
     * @brief This function serves 2 purposes: either fetch existing tiles from the cache or allocate new ones, or both at the same time.
     * This function tries to obtain tilesToAlloc.size() free tiles from the internal storage. If not available, grow the internal memory mapped file
     * so at least tilesToAlloc.size() free tiles are available.
     * @param tilesToAlloc A vector of size of the number of desired tiles in output. The numbers in the vector are used to offset the bucket of the 
     * cache on which to retrieve tiles from.
     * @param allocatedTilesData[out] In output, this contains each tiles allocated as a pair of <tileIndex, pointer>
     * Each tile will have exactly NATRON_TILE_SIZE_BYTES bytes. The index is the index that must be passed back to the unLockTiles
     * and releaseTiles functions.
     *
     * @param tileIndices List of existing tile indices for which we want to retrieve a pointer to. In output they will be set to existingTilesData
     * Note that the pointers returned in existingTilesData will be in the same order as the indices passed by tileIndices.
     *
     * The memory pointers returned by this function are guaranteed to be valid until you call unLockTiles after which they may be invalid.
     * To ensure the pointers are valid in output of this function, a mutex is taken internally by this function. Ensure that you call unLockTiles()
     * with the cacheData pointer returned from this function otherwise the program will deadlock.
     *
     * This function CANNOT be called in the implementation of CacheEntryBase::fromMemorySegment or CacheEntryBase::toMemorySegment otherwise this will
     * deadlock.
     *
     * @returns True upon success, false otherwise.
     * When returning false, you must still call unLockTiles, but do not have to call releaseTiles.
     * Note that unLockTiles must always be called before releaseTiles.
     **/
    virtual bool retrieveAndLockTiles(const CacheEntryBasePtr& entry,
                              const std::vector<U64>* tileIndices,
                              const std::vector<U64>* tilesToAlloc,
                              std::vector<void*>* existingTilesData,
                              std::vector<std::pair<U64, void*> >* allocatedTilesData,
                              void** cacheData) = 0;

#ifdef DEBUG
    /**
     * @brief Debug: Ensures that the index is valid in the storage. Can only be called between retrieveAndLockTiles and 
     * the corresponding call to unLockTiles
     **/
    virtual bool checkTileIndex(U64 encodedIndex) const = 0;
#endif

    /**
     * @brief Free cache data allocated from a call to retrieveAndLockTiles
     * This function CANNOT be called in the implementation of CacheEntryBase::fromMemorySegment or CacheEntryBase::toMemorySegment otherwise this will
     * deadlock.
     * Note this function does not free the memory allocated for the tiles, it just cleans up the mutex taken in retrieveAndLockTiles().
     * The memory will be freed when the cache entry is removed from the cache.
     **/
    virtual void unLockTiles(void* cacheData) = 0;

    /**
     * @brief Release tiles that were previously allocated by the given entry with retrieveAndLockTiles
     * @param localIndices Corresponds to the indices that were passed in tileIndices to the function retrieveAndLockTiles
     * @param cacheIndices Corresponds to the indices that were returned in allocatedTilesData in the function retrieveAndLockTiles
     **/
    virtual void releaseTiles(const CacheEntryBasePtr& entry, const std::vector<U64>& localIndices, const std::vector<U64>& cacheIndices) = 0;

    /**
     * @brief Returns whether a cache entry exists for the given hash.
     * This is significantly faster than the get() function but does not return the entry.
     **/
    virtual bool hasCacheEntryForHash(U64 hash) const = 0;

    /**
     * @brief Clears the cache of its last recently used entries so at least nBytesToFree are available for the given storage.
     * This should be called before allocating any buffer in the application to ensure we do not hit the swap.
     *
     * This function is not blocking and it is not guaranteed that the memory is available when returning. 
     * Evicted entries will be deleted in a separate thread so this thread can continue its own work.
     **/
    virtual void evictLRUEntries(std::size_t nBytesToFree) = 0;

    /**
     * @brief Clear the cache of entries that can be purged.
     **/
    virtual void clear() = 0;

    /**
     * @brief Removes this entry from the cache (if it exists in the cache)
     **/
    virtual void removeEntry(const CacheEntryBasePtr& entry) = 0;

    /**
     * @brief Flush the opened memory mapped files on disk to ensure their persistence.
     * This can be an expensive operation.
     * @param async If true, the data is not guaranteed to be flushed when returning the function,
     * otherwise this function does not return before all data is flushed.
     **/
    virtual void flushCacheOnDisk(bool async) = 0;

    /**
     * @brief Returns cache stats for each plug-in
     **/
    virtual void getMemoryStats(std::map<std::string, CacheReportInfo>* infos) const = 0;

#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    /**
     * @brief Scans the set of currently registered processes to check if they are still alive.
     * If a process is no longer active, it is removed from the mapped process list, potentially
     * unblocking pending cached entries.
     * This should be called periodically from a dedicated thread.
     **/
    virtual void cleanupMappedProcessList() = 0;

    /**
     * @brief Returns the uuid of the current process in the cache. This is only relevant for a persistent cache.
     **/
    virtual boost::uuids::uuid getCurrentProcessUUID() const = 0;

    /**
     * @brief Returns true if the given uuid is still registered in the mapped process set.
     **/
    virtual bool isUUIDCurrentlyActive(const boost::uuids::uuid& tag) const = 0;
#endif

};

/**
 * @brief An exception thrown when the cache is used by another process
 **/
class BusyCacheException : public std::exception
{

public:

    BusyCacheException()
    {
    }

    virtual ~BusyCacheException() throw()
    {
    }

    virtual const char * what () const throw ()
    {
        return "This cache is already used by another process";
    }
};

/**
 * @param persistent If true, the cache will be stored in memory mapped files to ensure
 * persistence on the file system. Note that in this case, only data structures that
 * can be held in shared memory can be inserted in the cache.
 * Note that currently there can be only a single persistent cache within Natron.
 *
 **/
template <bool persistent>
class Cache
: public CacheBase
{

    friend class CacheCleanerThread;
    friend struct CacheEntryLockerPrivate<persistent>;
    friend class CacheEntryLocker<persistent>;
    friend struct CacheBucket<persistent>;

    void initialize(const boost::shared_ptr<Cache<persistent> >& thisShared);

    Cache(bool enableTileStorage);

public:



    virtual ~Cache();


    /**
     * @brief Create a new instance of a cache
     * If the cache is persistent, this function may throw a BusyCacheException exception if the cache is used by another process
     **/
    static CacheBasePtr create(bool enableTileStorage);


    virtual bool isPersistent() const OVERRIDE FINAL;
    virtual std::string getCacheDirectoryPath() const OVERRIDE FINAL;
    virtual void setMaximumCacheSize(std::size_t size) OVERRIDE FINAL;
    virtual std::size_t getMaximumCacheSize() const OVERRIDE FINAL;
    virtual std::size_t getCurrentSize() const OVERRIDE FINAL;
    virtual CacheEntryLockerBasePtr get(const CacheEntryBasePtr& entry) const OVERRIDE FINAL;
    virtual bool retrieveAndLockTiles(const CacheEntryBasePtr& entry,
                                      const std::vector<U64>* tileIndices,
                                      const std::vector<U64>* tilesToAlloc,
                                      std::vector<void*>* existingTilesData,
                                      std::vector<std::pair<U64, void*> >* allocatedTilesData,
                                      void** cacheData) OVERRIDE FINAL;
#ifdef DEBUG
    virtual bool checkTileIndex(U64 encodedIndex) const OVERRIDE FINAL WARN_UNUSED_RETURN;
#endif
    virtual void unLockTiles(void* cacheData) OVERRIDE FINAL;
    virtual void releaseTiles(const CacheEntryBasePtr& entry, const std::vector<U64>& localIndices, const std::vector<U64>& cacheIndices) OVERRIDE FINAL;
    virtual bool hasCacheEntryForHash(U64 hash) const OVERRIDE FINAL;
    virtual void evictLRUEntries(std::size_t nBytesToFree) OVERRIDE FINAL;
    virtual void clear() OVERRIDE FINAL;
    virtual void removeEntry(const CacheEntryBasePtr& entry) OVERRIDE FINAL;
    virtual void flushCacheOnDisk(bool async) OVERRIDE FINAL;
    virtual void getMemoryStats(std::map<std::string, CacheReportInfo>* infos) const OVERRIDE FINAL;
#ifdef NATRON_CACHE_INTERPROCESS_ROBUST
    virtual void cleanupMappedProcessList() OVERRIDE FINAL;
    virtual boost::uuids::uuid getCurrentProcessUUID() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isUUIDCurrentlyActive(const boost::uuids::uuid& tag) const OVERRIDE FINAL WARN_UNUSED_RETURN;
#endif
private:

    boost::scoped_ptr<CachePrivate<persistent> > _imp;
};

NATRON_NAMESPACE_EXIT;


#endif /*Engine_Cache_h_ */
