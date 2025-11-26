<script>
  import { onMount } from "svelte";
  import { fade } from "svelte/transition";
  import { L } from "@lib/store.js";
  import * as utils from "@lib/utils.js";

  let selection = "current";
  let content = "";
  let loading = false;
  let error = null;
  let logFile = utils.DEFAULT_CONFIG.logfile;

  async function load() {
    loading = true;
    error = null;
    content = "";
    try {
      const cfg = await utils.loadConfig();
      logFile = cfg.logfile || utils.DEFAULT_CONFIG.logfile;

      content = await utils.fetchLog(logFile, selection === "old");
    } catch (e) {
      error = $L.logs.readFailed;
    } finally {
      loading = false;
    }
  }

  $: (selection, load());
</script>

<div class="card" in:fade={{ duration: 180 }}>
  <h2>{$L.logs.title}</h2>

  <div class="field">
    <label for="log-select">{$L.logs.select}</label>
    <div class="log-select-row">
      <select id="log-select" bind:value={selection}>
        <option value="current">{$L.logs.current}</option>
        <option value="old">{$L.logs.old}</option>
      </select>
      <button class="refresh-btn" on:click={load} disabled={loading}>
        {loading ? "..." : $L.logs.refresh}
      </button>
    </div>
  </div>

  {#if error}<p class="error">{error}</p>{/if}
  <pre class="log-view">{loading && !content
      ? "Loading..."
      : content || $L.logs.empty}</pre>
</div>
