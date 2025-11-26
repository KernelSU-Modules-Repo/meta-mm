<script>
  import { onMount } from "svelte";
  import { fade } from "svelte/transition";
  import { L } from "@lib/store.js";
  import * as utils from "@lib/utils.js";

  let modules = [];
  let loading = false;
  let error = null;
  // 简单的缓存 moduleDir，实际可以从 Config 获取或者再次读取 Config
  // 这里为了简单，我们假设使用默认或重新读取一次 Config 来获取路径，或者从 props 传入
  // 更好的方式是 Store 中存储 config，这里简化为重新获取
  let moduleDir = utils.DEFAULT_CONFIG.moduledir;

  async function load() {
    loading = true;
    error = null;
    try {
      // 确保获取最新的 moduleDir
      const cfg = await utils.loadConfig();
      moduleDir = cfg.moduledir || utils.DEFAULT_CONFIG.moduledir;
      modules = await utils.fetchModules(moduleDir);
    } catch (e) {
      error = $L.modules.loadError;
      console.error(e);
    } finally {
      loading = false;
    }
  }

  async function toggle(mod) {
    if (mod.disabledByFlag || mod.toggling) return;

    // Optimistic UI update
    const oldState = mod.skipMount;
    mod.toggling = true;
    modules = modules; // trigger update

    try {
      await utils.toggleModuleSkip(moduleDir, mod.name, !oldState);
      mod.skipMount = !oldState;
      mod.error = undefined;
    } catch (e) {
      mod.error = $L.modules.toggleError;
      mod.skipMount = oldState; // revert
    } finally {
      mod.toggling = false;
      modules = modules; // trigger update
    }
  }

  onMount(load);
</script>

<div class="card" in:fade={{ duration: 180 }}>
  <h2>{$L.modules.title}</h2>
  <p class="path">{$L.modules.basePath}: {moduleDir}</p>

  {#if error}<p class="error">{error}</p>{/if}
  {#if loading}<p class="hint">Loading...</p>{/if}

  {#if !loading && modules.length === 0 && !error}
    <p class="hint">{$L.modules.empty}</p>
  {/if}

  <div class="module-list">
    {#each modules as m (m.name)}
      <div class="module-row">
        <div class="module-info">
          <span class="module-name">{m.name}</span>
          <label class="switch" class:disabled={m.disabledByFlag}>
            <input
              type="checkbox"
              checked={!m.skipMount}
              disabled={m.disabledByFlag || m.toggling}
              on:change={() => toggle(m)}
            />
            <span class="slider"></span>
          </label>
        </div>
        {#if m.error}<div class="error small">{m.error}</div>{/if}
      </div>
    {/each}
  </div>

  <div class="actions" style="margin-top: 20px">
    <button on:click={load} disabled={loading}>
      {loading ? "Loading..." : $L.modules.reload}
    </button>
  </div>
</div>
