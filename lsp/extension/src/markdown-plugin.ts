import * as vscode from "vscode";
import type MarkdownIt from "markdown-it";
import * as shiki from "shiki";

function mapVscodeThemeToShikiTheme(
    vscodeKind: vscode.ColorThemeKind,
): "github-dark" | "github-light" {
    switch (vscodeKind) {
        case vscode.ColorThemeKind.Dark:
        case vscode.ColorThemeKind.HighContrast:
            return "github-dark";
        case vscode.ColorThemeKind.Light:
        case vscode.ColorThemeKind.HighContrastLight:
        default:
            return "github-light";
    }
}

export function getExtendMarkdownIt(context: vscode.ExtensionContext) {
    const createHighlighter = async () => {
        const uri = vscode.Uri.joinPath(
            context.extensionUri,
            "syntaxes",
            "nari.tmLanguage.json",
        );
        const bytes = await vscode.workspace.fs.readFile(uri);
        const grammar = JSON.parse(new TextDecoder().decode(bytes));

        return await shiki.createHighlighter({
            themes: ["github-dark", "github-light"],
            langs: [
                "markdown",
                {
                    ...grammar,
                    name: 'nari',
                    scopeName: 'source.nari',
                },
            ],
        });
    };

    return function extendMarkdownIt(md: MarkdownIt) {
        let highlighter: shiki.Highlighter | null = null;
        createHighlighter().then((h) => {
            highlighter = h;
        });

        const realHighlight = md.options.highlight ?? (() => "");

        md.options.highlight = (code: string, lang: string, attrs: string) => {
            if (lang === "nari" && highlighter) {
                try {
                    const activeShikiTheme = mapVscodeThemeToShikiTheme(
                        vscode.window.activeColorTheme.kind,
                    );
                    return highlighter.codeToHtml(code, {
                        lang: "nari",
                        theme: activeShikiTheme,
                        transformers: [
                            {
                                pre(node: any) {
                                    // Remove background style to match default markdown preview
                                    delete node.properties.style;
                                },
                            },
                        ],
                    });
                } catch {}
            }
            return realHighlight(code, lang, attrs);
        };
        return md;
    };
}
