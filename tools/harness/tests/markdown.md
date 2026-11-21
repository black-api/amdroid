# Markdown regression fixture

Render with: `bash tools/harness/build.sh render tools/harness/tests/markdown.md`

## Nested lists - inner list must be inside the parent <li>

- first
- second
  - nested bullet
    1. deep ordered
    2. deep ordered two
  - nested bullet two
- third

1. ordered one
2. ordered two
   - bullet under ordered
3. ordered three

## Nested quotes - returning to the outer level must start a new block

> outer paragraph
> > inner quote paragraph
> back in the outer quote

> another outer quote
> - a list in a quote
> - another item
> end of the quote
