// update/*.txt are bundled as text (Wrangler's default rule for .txt).
declare module "*.txt" {
  const text: string;
  export default text;
}

// tests: a file as text (Vite's ?raw)
declare module "*?raw" {
  const text: string;
  export default text;
}
