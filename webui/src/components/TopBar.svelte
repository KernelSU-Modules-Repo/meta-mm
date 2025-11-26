<script>
  import { lang, availableLanguages } from "@lib/store.js";

  let dropdownOpen = false;

  function selectLanguage(code) {
    $lang = code;
    dropdownOpen = false;
  }

  function handleClickOutside(event) {
    if (dropdownOpen && !event.target.closest(".lang-dropdown")) {
      dropdownOpen = false;
    }
  }
</script>

<svelte:window on:click={handleClickOutside} />

<div class="top-bar">
  <div class="title">Magic Mount</div>
  <div class="lang-dropdown">
    <button
      type="button"
      class="lang-select"
      on:click|stopPropagation={() => (dropdownOpen = !dropdownOpen)}
    >
      {availableLanguages.find((l) => l.code === $lang)?.name || $lang}
      <span class="arrow {dropdownOpen ? 'open' : ''}">▼</span>
    </button>

    {#if dropdownOpen}
      <div class="lang-menu">
        {#each availableLanguages as l}
          <button
            class="lang-option {l.code === $lang ? 'active' : ''}"
            on:click={() => selectLanguage(l.code)}
          >
            {l.name}
          </button>
        {/each}
      </div>
    {/if}
  </div>
</div>
