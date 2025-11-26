<script>
  import { onMount } from "svelte";
  import { fade } from "svelte/transition";
  import { L } from "@lib/store.js";
  import * as utils from "@lib/utils.js";

  let config = { ...utils.DEFAULT_CONFIG };
  let loading = false;
  let saving = false;
  let message = null;
  let partitionsInput = "";

  const CONFIG_FILE_PATH = "/data/adb/magic_mount/mm.conf";

  async function load() {
    loading = true;
    message = null;
    try {
      config = await utils.loadConfig();
      partitionsInput = config.partitions.join(", ");
      message = $L.config.loadSuccess;
    } catch (e) {
      console.error(e);
      message = $L.config.loadError;
    } finally {
      loading = false;
    }
  }

  async function save() {
    saving = true;
    message = null;
    try {
      config.partitions = partitionsInput
        .split(",")
        .map((s) => s.trim())
        .filter(Boolean);
      await utils.saveConfig(config);
      message = $L.config.saveSuccess;
    } catch (e) {
      console.error(e);
      message = $L.config.saveFailed;
    } finally {
      saving = false;
    }
  }

  onMount(load);
</script>

<div class="card" in:fade={{ duration: 180 }}>
  <h2>{$L.config.title}</h2>

  {#if loading}<p class="hint">Loading...</p>{/if}
  {#if message}<p class="hint">{message}</p>{/if}

  <div class="field">
    <span class="field-label">{$L.config.verboseLabel}</span>
    <div class="loglevel-switch">
      <button
        type="button"
        class="lv-btn {!config.verbose ? 'active' : ''}"
        on:click={() => (config.verbose = false)}>{$L.config.verboseOff}</button
      >
      <button
        type="button"
        class="lv-btn {config.verbose ? 'active' : ''}"
        on:click={() => (config.verbose = true)}>{$L.config.verboseOn}</button
      >
    </div>
  </div>

  <div class="field">
    <span class="field-label">{$L.config.umountLabel}</span>
    <div class="loglevel-switch">
      <button
        type="button"
        class="lv-btn {!config.umount ? 'active' : ''}"
        on:click={() => (config.umount = false)}>{$L.config.umountOff}</button
      >
      <button
        type="button"
        class="lv-btn {config.umount ? 'active' : ''}"
        on:click={() => (config.umount = true)}>{$L.config.umountOn}</button
      >
    </div>
  </div>

  <div class="field">
    <label class="field-label" for="inp-moduledir">{$L.config.moduleDir}</label>
    <input
      id="inp-moduledir"
      type="text"
      bind:value={config.moduledir}
      placeholder={utils.DEFAULT_CONFIG.moduledir}
    />
  </div>

  <div class="field">
    <label class="field-label" for="inp-tempdir">{$L.config.tempDir}</label>
    <input
      id="inp-tempdir"
      type="text"
      bind:value={config.tempdir}
      placeholder="(auto)"
    />
  </div>

  <div class="field">
    <label class="field-label" for="inp-mountsource"
      >{$L.config.mountSource}</label
    >
    <input id="inp-mountsource" type="text" bind:value={config.mountsource} />
  </div>

  <div class="field">
    <label class="field-label" for="inp-logfile">{$L.config.logFile}</label>
    <input id="inp-logfile" type="text" bind:value={config.logfile} />
  </div>

  <div class="field">
    <label class="field-label" for="inp-partitions"
      >{$L.config.extPartitions}</label
    >
    <input
      id="inp-partitions"
      type="text"
      bind:value={partitionsInput}
      placeholder="eg. mi_ext,my_stock"
    />
  </div>

  <div class="actions">
    <button type="button" on:click={load} disabled={loading || saving}
      >{$L.config.reload}</button
    >
    <button
      type="button"
      class="primary"
      on:click={save}
      disabled={loading || saving}
    >
      {saving ? "Saving..." : $L.config.save}
    </button>
  </div>

  <p class="path">{$L.config.pathLabel}: {CONFIG_FILE_PATH}</p>
</div>
