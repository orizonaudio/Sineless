// Lint rules; naming rules encode DESIGN 8.3.
import js from "@eslint/js";
import tseslint from "typescript-eslint";

const kebabCaseFile = /^[a-z0-9]+(?:-[a-z0-9]+)*(?:\.[a-z0-9]+)*\.ts$/;

const local = {
  rules: {
    "kebab-case-filename": {
      meta: { type: "suggestion", schema: [] },
      create(context) {
        const name = context.filename.split(/[\\/]/).pop() ?? "";
        if (!kebabCaseFile.test(name)) {
          context.report({
            loc: { line: 1, column: 0 },
            message: `File name '${name}' must be kebab-case.`,
          });
        }
        return {};
      },
    },
  },
};

export default tseslint.config(
  { ignores: ["build/", "node_modules/"] },
  js.configs.recommended,
  {
    files: ["**/*.ts"],
    extends: [tseslint.configs.strictTypeChecked],
    languageOptions: {
      parserOptions: {
        projectService: true,
        tsconfigRootDir: import.meta.dirname,
      },
    },
    plugins: { local },
    rules: {
      "local/kebab-case-filename": "error",
      "@typescript-eslint/naming-convention": [
        "error",
        { selector: "default", format: ["camelCase"] },
        { selector: "typeLike", format: ["PascalCase"] },
        // Enum-like `as const` objects are PascalCase.
        {
          selector: "variable",
          modifiers: ["const", "global"],
          format: ["camelCase", "PascalCase"],
        },
        { selector: "import", format: ["camelCase", "PascalCase"] },
        {
          selector: "objectLiteralProperty",
          modifiers: ["requiresQuotes"],
          format: null,
        },
      ],
    },
  },
);
