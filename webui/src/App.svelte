<script>
  import "@/app.css";
  import { L } from "@lib/store.js";

  // Components
  import TopBar from "@comp/TopBar.svelte";
  import ConfigView from "@comp/ConfigView.svelte";
  import ModulesView from "@comp/ModulesView.svelte";
  import LogsView from "@comp/LogsView.svelte";

  // Tabs
  const tabs = {
    config: { component: ConfigView, label: "tabs.config" }, // label 为 locate key
    module: { component: ModulesView, label: "tabs.modules" },
    log: { component: LogsView, label: "tabs.logs" },
  };

  let activeTab = "config";
  let isSwitching = false;

  function switchTab(key) {
    if (activeTab === key || isSwitching) return;
    isSwitching = true;
    setTimeout(() => {
      activeTab = key;
      isSwitching = false;
    }, 200);
  }
</script>

<div class="app-root">
  <TopBar />

  <div class="app-main">
    {#if isSwitching}
      <div class="tab-overlay"><div class="tab-spinner"></div></div>
    {/if}

    {#key activeTab}
      <svelte:component this={tabs[activeTab].component} />
    {/key}
  </div>

  <div class="bottom-bar">
    {#each Object.entries(tabs) as [key, item]}
      <button
        type="button"
        class="tab-btn {activeTab === key ? 'active' : ''}"
        on:click={() => switchTab(key)}
      >
        {$L[item.label.split(".")[0]][item.label.split(".")[1]]}
      </button>
    {/each}
  </div>
</div>
