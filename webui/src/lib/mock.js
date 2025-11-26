const delay = (ms = 500) => new Promise((resolve) => setTimeout(resolve, ms));

export const MockAPI = {
  async loadConfig(defaultConfig) {
    console.log("[Dev] Load Mock config...");
    await delay();

    return {
      ...defaultConfig,
      moduledir: "/data/adb/modules",
      partitions: ["mi_ext", "my_stock"],
      verbose: true,
      umount: false,
    };
  },

  async saveConfig(config) {
    console.log("[Dev] Save Mock config:", config);
    await delay(800);

    if (Math.random() > 0.9) {
      throw new Error("Save error：IO error");
    }
    return true;
  },

  async fetchLog(path, isOld) {
    console.log(`[Dev] Fetch log: ${path} (old=${isOld})`);
    await delay();

    if (isOld) {
      return `[Old Log]
Nothing interesting here.
Just some history data.`;
    }

    return `[INFO] main.c:72: Loading config file: /data/adb/magic_mount/mm.conf
[DEBUG] module_tree.c:226: extra_partition_register: processing 'my_stock' (len=8)
[INFO] module_tree.c:259: extra_partition_register: success added 'my_stock' (total: 1 partitions)
[DEBUG] main.c:152: Added extra partition: my_stock
[DEBUG] module_tree.c:226: extra_partition_register: processing 'sys' (len=3)
[WARN] module_tree.c:244: extra_partition_register: rejected 'sys' (blacklisted)
[DEBUG] main.c:152: Added extra partition: test
[INFO] utils.c:283: auto tempdir selected: /mnt/vendor/.magic_mount (from /mnt/vendor)
[INFO] main.c:336: Magic Mount mock Starting
[INFO] main.c:337: Configuration:
[INFO] main.c:338:   Module directory:  /data/adb/modules
[INFO] main.c:339:   Temp directory:    /mnt/vendor/.magic_mount
[INFO] main.c:340:   Mount source:      KSU
[INFO] main.c:341:   Log level:         DEBUG
[INFO] main.c:343:   Extra partitions:  1
[INFO] main.c:345:     - custom_part
[INFO] module_tree.c:663: build_mount_tree: module_dir=/data/adb/modules`;
  },

  async fetchModules(moduleDir) {
    console.log(`[Dev] Scan modules: ${moduleDir}`);
    await delay();

    return [
      { name: "mod1", disabledByFlag: false, skipMount: false },
      { name: "mod2", disabledByFlag: false, skipMount: true },
      { name: "mod3", disabledByFlag: true, skipMount: false },
      { name: "mod4", disabledByFlag: false, skipMount: false },
      { name: "mod5", disabledByFlag: false, skipMount: false },
    ];
  },

  async toggleModuleSkip(moduleDir, modName, shouldSkip) {
    console.log(`[Dev] Switch module ${modName} skip_mount=${shouldSkip}`);
    await delay(300);
    return true;
  },
};
