import { writable, derived } from "svelte/store";
import locate from "./locate.json";

const getInitialLang = () => {
  if (typeof window !== "undefined") {
    return window.localStorage.getItem("mm-lang") || "en";
  }
  return "en";
};

export const lang = writable(getInitialLang());

lang.subscribe((val) => {
  if (typeof window !== "undefined") {
    window.localStorage.setItem("mm-lang", val);
  }
});

export const L = derived(lang, ($lang) => locate[$lang] || locate["en"]);

export const availableLanguages = Object.keys(locate).map((code) => ({
  code,
  name: locate[code]?.lang?.display || code.toUpperCase(),
}));
