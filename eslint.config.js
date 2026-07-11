import eslint from "@eslint/js";
import { defineConfig, globalIgnores } from "eslint/config";
import tseslint from "typescript-eslint";

export default defineConfig(
    globalIgnores([
        "**/.build/**",
        "**/.gradle/**",
        "**/.tools/**",
        "**/build/**",
        "**/coverage/**",
        "**/dist/**",
        "**/generated/**",
        "**/node_modules/**"
    ]),
    eslint.configs.recommended,
    tseslint.configs.recommendedTypeChecked,
    {
        files: ["**/*.{js,mjs,cjs}"],
        extends: [tseslint.configs.disableTypeChecked]
    },
    {
        files: ["packages/es-markdown-core/src/**/*.ts"],
        languageOptions: {
            globals: {
                URL: "readonly",
                WebAssembly: "readonly",
                TextDecoder: "readonly",
                TextEncoder: "readonly",
                fetch: "readonly"
            }
        }
    },
    {
        files: ["packages/es-markdown-core/{scripts,tests}/**/*.{js,mjs}"],
        languageOptions: {
            globals: {
                Buffer: "readonly",
                URL: "readonly",
                clearTimeout: "readonly",
                console: "readonly",
                process: "readonly",
                setTimeout: "readonly"
            }
        }
    },
    {
        files: ["scripts/**/*.{js,mjs,cjs}"],
        languageOptions: {
            globals: {
                process: "readonly"
            }
        }
    },
    {
        files: ["packages/es-markdown-core/tests/types/**/*.ts"],
        extends: [tseslint.configs.disableTypeChecked]
    },
    {
        files: ["**/*.{ts,tsx,mts,cts}"],
        languageOptions: {
            parserOptions: {
                projectService: true,
                tsconfigRootDir: import.meta.dirname
            }
        }
    }
);
