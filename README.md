Littlefs port
-------------

Uses an external littlefs repo.  The directory structure I picked is
different from what Zephyr ended up adopting when it started moving
code to external modules; `modules/fs/littlefs` is probably better
than `ext/fs/littlefs/src` but I'm not sure where the other stuff in
`ext/fs/littlefs` should go.  My `west.yml` contains:

    remotes:
      - name: armmbed
        url-base: https://github.com/ARMmbed

    projects:
      - name: littlefs
        path: zephyr/ext/fs/littlefs/src
        remote: armmbed
        revision: c31d118a1f2466a362dc9f4f3472ece423a7670f

The code expects partitions and my devicetree contains two:

    &spi1 {
            status = "ok";
            sck-pin = <11>;
            mosi-pin = <9>;
            miso-pin = <10>;
            cs-gpios = <&gpio0 14 0>;

            spiflash0: spiflash@0 {
                    compatible = "jedec,spi-nor";
                    label = "EXT_FLASH";
                    reg = <0>;
                    spi-max-frequency = <104000000>;

                    /* GigaDevice GD25Q127C */
                    jedec-id = <0xc8 0x40 0x18>;
                    size = <(128 * 1024 * 1024)>; /* bits */
                    erase-block-size = <4096>;
                    write-block-size = <16>;

                    partitions {
                            compatible = "fixed-partitions";
                            #address-cells = <1>;
                            #size-cells = <1>;

                            data_partition: partition@0 {
                                    label = "data";
                                    reg = <0x00000000 0x010000>;
                            };
                            log_partition: partition@10000 {
                                    label = "log";
                                    reg = <0x00010000 0xff0000>;
                            };
                    };
            };
    };

I mount partitions as follows:

    #ifdef CONFIG_FILE_SYSTEM_LITTLEFS
    #include <fs/littlefs.h>

    static struct fs_littlefs_t lfs_data;
    static struct fs_mount_t lfs_data_mnt = {
    	.type = FS_LITTLEFS,
    	.fs_data = &lfs_data,
            .storage_dev = (void *)DT_FLASH_AREA_DATA_ID,
            .mnt_point = "/data",
    };

    static struct fs_littlefs_t lfs_log;
    static struct fs_mount_t lfs_log_mnt = {
    	.type = FS_LITTLEFS,
    	.fs_data = &lfs_log,
            .storage_dev = (void *)DT_FLASH_AREA_LOG_ID,
            .mnt_point = "/log",
    };

    static int storage_init(struct device *dev)
    {
            int ret;

    	ARG_UNUSED(dev);

            /* Mount littlefs */
            ret = fs_mount(&lfs_data_mnt);
            if (ret != 0) {
                    printk("Error mounting LittleFS (data partition)\n");
                    return ret;
            }

            ret = fs_mount(&lfs_log_mnt);
            if (ret != 0) {
                    printk("Error mounting LittleFS (log partition)\n");
                    return ret;
            }

            /* Silence super verbose FS errors (local helper that loops
               through log_source_name_get() and calls log_filter_set()) */
            //log_level_set("fs", LOG_LEVEL_NONE);

            return 0;
    }
    SYS_INIT(storage_init, APPLICATION, 50);
    #endif
