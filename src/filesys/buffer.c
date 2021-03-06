#include <bitmap.h>
#include <debug.h>
#include <hash.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "filesys/buffer.h"
#include "filesys/filesys.h"
#include "threads/synch.h"

#define BUFFER_SIZE 64
#define UNOCCUPIED UINT32_MAX


// OBJECT DEFINITIONS
struct buffer_entry {
	// hash_elem for the buffer_table
	struct hash_elem hash_elem;

	// The block that currently occupies this cache slot. Used as the
	// key in the hash table.
	block_sector_t occupied_by_sector;

	// If this cache entry has been modified and hasn't
	// been written back to disk yet.
	bool dirty;

	// If this cache entry has been read from or written to since
	// the last eviction algorithm pass
	bool recently_accessed;

    // 512 bytes of block data
	uint8_t storage[BLOCK_SECTOR_SIZE];
    
    // The lock that allows multiple threads to read from but
    // only one thread to write to the storage at once.
	struct lock lock;
};


// GLOBAL VARIABLES
static struct buffer_entry buffer[BUFFER_SIZE];
static uint8_t buffer_unoccupied_slots;

// This hash below maps disk sector to buffer position
static struct hash buffer_table;
static struct lock buffer_table_lock;

// This is a number, ranging between 0 and BUFFER_SIZE,
// that represents where the eviction clock algorithm is
// pointing in the array of buffer elements right now.
static int eviction_clock_position;


// BUFFER HASH TABLE FUNCTIONS
bool buffer_less(const struct hash_elem *a,
               const struct hash_elem *b,
               void *aux UNUSED) {
    struct buffer_entry *buffer_entry_a = hash_entry(a, struct buffer_entry, hash_elem);
    struct buffer_entry *buffer_entry_b = hash_entry(b, struct buffer_entry, hash_elem);
    
    return buffer_entry_a->occupied_by_sector < buffer_entry_b->occupied_by_sector;
}
unsigned buffer_hash(const struct hash_elem *e, void *aux UNUSED) {
    struct buffer_entry *b = hash_entry(e, struct buffer_entry, hash_elem);
    
    return hash_bytes(&b->occupied_by_sector, sizeof(b->occupied_by_sector));
}


// BUFFER FUNCTIONS
void buffer_init(void) {
	/* A brief note about how we handle unoccupied slots.
       Since slots are only unoccupied for a moment at the very
       beginning and then occupied forevermore, we just keep track
       of how many unoccupied slots are left, counting down from 64 to 0.
       While there are unoccupied slots left, we just fill them up in
       order from 63 to 0. Once there are no unoccupied slots left
       (i.e. unoccupied_slots == 0) then we start evicting things. */
	buffer_unoccupied_slots = BUFFER_SIZE;

	hash_init(&buffer_table, buffer_hash, buffer_less, NULL);

	lock_init(&buffer_table_lock);

	eviction_clock_position = 0;

	int i;
	for (i = 0; i < BUFFER_SIZE; i++) {
		buffer[i].occupied_by_sector = UNOCCUPIED;
		buffer[i].dirty = false;
		buffer[i].recently_accessed = false;
		lock_init(&buffer[i].lock);
	}
}

// Precondition: The buffer entry's lock is held by the current thread.
void writeback_dirty_buffer_entry(struct buffer_entry* b) {
	ASSERT(lock_held_by_current_thread(&(b->lock)));
	ASSERT(b->dirty == true);
	block_write(fs_device, b->occupied_by_sector, &(b->storage));
	b->dirty = false;
}

// This function just writes back all dirty entries in the cache.
void buffer_flush(void) {
	// This function makes no effort to prevent writes while it's
	// running, except for the block currently being written back.
	// We thought about it and couldn't think of any reason it'd be helpful.

	int i = 0;
	for (i = 0; i < BUFFER_SIZE; i++) {
		lock_acquire(&(buffer[i].lock));
		if (buffer[i].dirty) {
			writeback_dirty_buffer_entry(&buffer[i]);
		}
		lock_release(&(buffer[i].lock));
	}
}

// This accepts a sector number and looks up the buffer_entry struct for that
// sector. Returns null if the given sector isn't in the cache.
// Precondition: Must hold the buffer_table_lock.
struct buffer_entry *_buffer_entry_for_sector(block_sector_t sector) {
    ASSERT(lock_held_by_current_thread(&buffer_table_lock)); 

    // Create dummy entry for lookup
    struct buffer_entry lookup_entry;
    lookup_entry.occupied_by_sector = sector;
    
    // Get the buffer_entry associated with the given page.
    struct hash_elem *e = hash_find(&buffer_table, &lookup_entry.hash_elem);
    if (e == NULL) {
        return NULL;
    } else {
        return hash_entry(e, struct buffer_entry, hash_elem);
    }
}

// This accepts a sector number and looks up the buffer_entry struct for that
// sector. Returns null if the given sector isn't in the cache.
// Precondition: The global lock is held by the current thread.
// Postcondition: The global lock is released only if a buffer entry is returned.
struct buffer_entry *buffer_acquire_existing_entry(block_sector_t sector) {
    ASSERT(lock_held_by_current_thread(&buffer_table_lock));
    
    // First grab the entry from the sector.
    struct buffer_entry* b = _buffer_entry_for_sector(sector);
    
    // The buffer doesn't contain an entry for this sector.
    // Return WITHOUT releasing the global lock!
    if (b == NULL) return NULL;

    // We found it, so let's avoid locking on the global lock.
    // And make sure nobody is trying to steal our buffer entry!
    lock_release(&buffer_table_lock);
    lock_acquire(&b->lock);
    
    // Is this the same buffer entry we thought we retrieved?
    while (b->occupied_by_sector != sector) {
        // Nope, guess someone swapped it out. Rude.
        
        // Let go of this imposter and grab the global lock.
        lock_release(&b->lock);
        lock_acquire(&buffer_table_lock);
        
        // Let's try that again..
        b = _buffer_entry_for_sector(sector);
        
        // We found it, so let's avoid locking on the global lock.
        // And make sure nobody is trying to steal our buffer entry!
        lock_release(&buffer_table_lock);
        lock_acquire(&b->lock);
    }
    
    // Guess we acquired it.
    return b;
}

// Returns a pointer to an empty buffer slot.
// If there isn't one, takes care of evicting one.
// Precondition: The global lock is held by the current thread.
// Postcondition: The global lock and the buffer entry's lock are held
//                by the current thread.
struct buffer_entry* buffer_acquire_free_slot(void) {

	// Any function that calls this function should have already
	// acquired the lock on the buffer table.
	ASSERT(lock_held_by_current_thread(&buffer_table_lock));

    struct buffer_entry *b;
	// Check if there's any unoccupied slots
	if (buffer_unoccupied_slots > 0) {
		// The first unoccupied slot we'll fill is index 63, the last is 0
		buffer_unoccupied_slots -= 1;
        b = &(buffer[buffer_unoccupied_slots]);
        lock_acquire(&b->lock);
        ASSERT(b->occupied_by_sector == UNOCCUPIED);
	}
    // Otherwise, evict an existing buffer entry...
    else {
        b = &buffer[eviction_clock_position];

        // If a buffer slot is either recently accessed or locked,
        // we just keep looking.
        while (b->recently_accessed || !lock_try_acquire(&b->lock)) {
        	b->recently_accessed = false;
        	eviction_clock_position += 1;
        	eviction_clock_position %= 64;
        	b = &buffer[eviction_clock_position];
        }

        // lock_acquire(&b->lock);
        if (b->dirty) {
        	writeback_dirty_buffer_entry(b);
        }
        hash_delete(&buffer_table, &(b->hash_elem));
        b->occupied_by_sector = UNOCCUPIED;
        b->recently_accessed = false;
    }
    return b;
}

// Precondition: The global lock isn't held.
// Postcondition: The global lock isn't held, but the entry lock is.
struct buffer_entry *buffer_acquire(block_sector_t sector) {
    // Acquire the global lock. Will be automatically released
    // when an existing entry or free slot is acquired.
    lock_acquire(&buffer_table_lock);
    
    // First, let's see if sector is aleady in the buffer.
    struct buffer_entry *b = buffer_acquire_existing_entry(sector);
    
    // If this sector isn't yet loaded, load it here...
    if (b == NULL) {        
        // Grab a buffer entry, lock acquired, and set it up.
        b = buffer_acquire_free_slot();
        b->occupied_by_sector = sector;
        b->recently_accessed = true;
        hash_insert(&buffer_table, &(b->hash_elem));
        lock_release(&buffer_table_lock);

        // Read the on-disk data into the buffer.
        block_read(fs_device, sector, &(b->storage));
    }
    
    return b;
}

// Precondition: The entry lock is held.
// Postcondition: The global lock isn't held, but the entry lock is.
void buffer_release(struct buffer_entry *b) {
    lock_release(&(b->lock));
}

// This function is almost always used with its convenience method,
// buffer_read, which gets BLOCK_SECTOR_SIZE bytes.
// This function reads up to 512 bytes from the cache.
void buffer_read_bytes(block_sector_t sector, off_t sector_ofs, size_t num_bytes, void* buffer) {
	ASSERT(sector_ofs >= 0 && sector_ofs < BLOCK_SECTOR_SIZE);
	ASSERT(num_bytes > 0 && num_bytes <= BLOCK_SECTOR_SIZE);

    // Obtain buffer entry with that sector. This function
    // abstracts away a lot of important things, read it.
    struct buffer_entry *b = buffer_acquire(sector);
    
    // Copy the data into the buffer
    void* start = (void *)(((uint8_t *)(&(b->storage))) + sector_ofs);
    memcpy(buffer, start, num_bytes);

    // Release the lock on the buffer.
    buffer_release(b);
}
void buffer_read(block_sector_t sector, void* buffer) {
	buffer_read_bytes(sector, 0, BLOCK_SECTOR_SIZE, buffer);
}

// This function is almost always used with its convenience method,
// buffer_read, which gets BLOCK_SECTOR_SIZE bytes.
// This function writes up to 512 bytes to the cache.
void buffer_write_bytes(block_sector_t sector, off_t sector_ofs, size_t num_bytes, const void* buffer) {
	ASSERT(sector_ofs >= 0 && sector_ofs < BLOCK_SECTOR_SIZE);
	ASSERT(num_bytes > 0 && num_bytes <= BLOCK_SECTOR_SIZE);

    // Obtain buffer entry with that sector. This function
    // abstracts away a lot of important things, read it.
    struct buffer_entry *b = buffer_acquire(sector);

    // Copy the buffer data into our cache.
    void* start = (void *)(((uint8_t *)(&(b->storage))) + sector_ofs);
    memcpy(start, buffer, num_bytes);
    b->dirty = true;
    
    // Release the lock on the buffer.
    buffer_release(b);
}
void buffer_write(block_sector_t sector, const void* buffer) {
	buffer_write_bytes(sector, 0, BLOCK_SECTOR_SIZE, buffer);
}
