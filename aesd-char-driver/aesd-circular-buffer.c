/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer implementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to initialize
 */
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer, 0, sizeof(struct aesd_circular_buffer));
}

/**
 * @param buffer the buffer to add to
 * @param add_entry a pointer to the data to add
 */
const char *aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    const char *ret_ptr = NULL; // Pointer to store overwritten buffer

    if (buffer == NULL || add_entry == NULL) {
        return ret_ptr;
    }
    
    // If the buffer is full, we need to capture the buffer we are about to overwrite
    if (buffer->full) {
        // Capture the pointer of the entry we are about to overwrite
        ret_ptr = buffer->entry[buffer->in_offs].buffptr;
    }
    
    // Save the new entry at the current in_offs position
    buffer->entry[buffer->in_offs] = *add_entry;
    
    // Advance the in_offs
    buffer->in_offs = (buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    
    // If buffer was full, advance out_offs as well
    if (buffer->full) {
        buffer->out_offs = (buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    
    // Check if buffer is now full
    if (buffer->in_offs == buffer->out_offs) {
        buffer->full = true;
    }

    return ret_ptr; // Return pointer to overwritten buffer (or NULL if not full yet)
}


/**
 * @param buffer the buffer to find in
 * @param char_offset the position to search for in the buffer
 * @param entry_offset_byte_rtn a pointer to a location to store the entry offset within the entry
 * @return the buffer entry where the char_offset is found, or NULL if not found
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn)
{
    if (buffer == NULL || entry_offset_byte_rtn == NULL) {
        return NULL;
    }
    
    // Count of valid entries
    uint8_t count = buffer->full ? AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED : buffer->in_offs;
    
    // Track the current position in the buffer as we iterate
    size_t current_offset = 0;
    
    // Iterate through valid entries starting from the oldest (out_offs)
    for (uint8_t i = 0; i < count; i++) {
        uint8_t index = (buffer->out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        struct aesd_buffer_entry *entry = &buffer->entry[index];
        
        // Check if the requested offset falls within this entry
        if (current_offset <= char_offset && char_offset < current_offset + entry->size) {
            // Found the entry containing the requested offset
            *entry_offset_byte_rtn = char_offset - current_offset;
            return entry;
        }
        
        // Move to the next entry
        current_offset += entry->size;
    }
    
    // Offset not found in any entry
    return NULL;
}
