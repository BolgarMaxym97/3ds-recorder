// update/*.txt are bundled as text (Wrangler's default rule for .txt).
declare module "*.txt" {
  const text: string;
  export default text;
}
