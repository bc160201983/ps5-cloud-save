# Project working preferences

- Publish requested development changes to GitHub `main`; do not create another
  development branch unless the user requests one.
- Keep VERSION, CHANGELOG.md and README.md updated with each meaningful release.
  CHANGELOG.md gets a section per release; README.md is the project's front page
  (overview, features, getting started) and its Status table must distinguish tested
  console behavior from implemented but unverified features.
- Keep console credentials and real saves out of Git. Preserve original saves;
  stop restore when destination identity or game closure is unknown.
