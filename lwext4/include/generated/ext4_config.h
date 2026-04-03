#ifndef EXT4_CONFIG_H_
#define EXT4_CONFIG_H_

#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#define CONFIG_DEBUG_PRINTF         0
#define CONFIG_DEBUG_ASSERT         1
#define CONFIG_BLOCK_DEV_CACHE_SIZE 16
#define CONFIG_EXTENT_ENABLE        1
#define CONFIG_XATTR_ENABLE         1
#define CONFIG_JOURNALING_ENABLE    0
#define CONFIG_HAVE_OWN_ERRNO       0
#define CONFIG_HAVE_OWN_ASSERT      0

#ifndef EOK
#define EOK 0
#endif

#endif /* EXT4_CONFIG_H_ */
