# End-to-End Packaging Example (Library + App)

This cookbook example mirrors the end-to-end flow in
[`docs/plan/eta_packaging_plan.md`](../../../docs/plan/eta_packaging_plan.md).

## Layout

- `mathx/` - library package exporting `square` and `cube`
- `myapp/` - app package importing `mathx`

## Run the example

```console
cd cookbook/packaging/end-to-end/mathx
eta test

cd ../myapp
eta build
eta run
eta tree
```

Expected `eta run` output:

```text
49
```

## See also

- [How to Build Your First App](../../../docs/app/first_app.md)
- [Packaging System](../../../docs/packaging.md)
