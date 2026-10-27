# Publishing the repository

The intended GitHub owner is **sandeepkrishnams**. The local remote is:

```text
https://github.com/sandeepkrishnams/smok-glass-nautilus.git
```

Before using the GitHub CLI, verify its active account. The desktop plugin and `gh` can use different accounts.

```bash
gh api user --jq .login
```

If needed, sign in or switch the CLI to `sandeepkrishnams`. Once the active account is correct, create the public repository and push the existing local commit:

```bash
gh repo create sandeepkrishnams/smok-glass-nautilus --public \
  --description "Nautilus with read-only document, spreadsheet, image and Markdown previews"
git push -u origin main
```

This guide does not itself create or publish a GitHub repository. After publication, enable private vulnerability reporting in the repository settings and review the first CI run.
