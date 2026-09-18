# Resatoshi translations

Submit translation changes as pull requests to this repository. Identify the
language, affected UI text, and the source revision used for the translation.
The bundled translations originated in Bitcoin Core; its Transifex project
is an upstream source rather than a Resatoshi submission channel.

## Files and extraction

Qt translations use `src/qt/locale/bitcoin_xx.ts` or
`src/qt/locale/bitcoin_xx_YY.ts`. The English extraction file is
`src/qt/locale/bitcoin_en.ts`; non-Qt strings are collected in
`src/qt/bitcoinstrings.cpp`. These filenames remain unchanged.

With the Qt tools and gettext installed, regenerate source strings with:

```sh
cmake --preset dev-mode -DWITH_USDT=OFF -DENABLE_IPC=OFF
cmake --build build_dev_mode --target translate
```

Review the generated changes and include only the intended translation work.
Keep placeholders such as `%1`, `%n`, and format specifiers intact. Check plural
forms in Qt Linguist and test the affected dialogs with the selected language.
For a new language, follow the existing entries in `src/qt/bitcoin_locale.qrc`
and the locale build configuration.

See [translation string policy](translation_strings_policy.md) for source-code
conventions and [CONTRIBUTING.md](../CONTRIBUTING.md) for submission steps.
