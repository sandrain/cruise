#define SCRMFS_MAX_FILES        ( 128 )

/* eventually could decouple these so there could be
 * more or less file descriptors than files, but for
 * now they're the same */
#define SCRMFS_MAX_FILEDESCS    ( SCRMFS_MAX_FILES )

#define SCRMFS_MAX_FILENAME     ( 128 )

#ifdef MACHINE_BGQ
  #define SCRMFS_MAX_MEM          ( 32 * 1024 * 1024 )
#else /* MACHINE_BGQ */
  //#define SCRMFS_MAX_MEM          ( 1 * 1024 * 1024 * 1024 )
  #define SCRMFS_MAX_MEM          ( 256 * 1024 * 1024 )
#endif /* MACHINE_BGQ */

#define SCRMFS_SPILLOVER_SIZE   ( 1 * 1024 * 1024 * 1024 )

#define SCRMFS_CHUNK_BITS       ( 24 )
#define SCRMFS_CHUNK_SIZE       ( 1 << SCRMFS_CHUNK_BITS )
#define SCRMFS_CHUNK_MASK       ( SCRMFS_CHUNK_SIZE - 1 )
#define SCRMFS_MAX_CHUNKS       ( SCRMFS_MAX_MEM >> SCRMFS_CHUNK_BITS )
#define SCRMFS_MAX_SPILL_CHUNKS ( SCRMFS_SPILLOVER_SIZE >> SCRMFS_CHUNK_BITS )

#define SCRMFS_SUPERBLOCK_KEY   ( 4321 )
