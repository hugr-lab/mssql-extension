import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// Docs for the hugr-lab DuckDB MSSQL extension. Mirrors the org site's
// Docusaurus setup (hugr-lab.github.io) so the two share look and feel;
// published by this repo's Pages workflow to /mssql-extension alongside the
// download-metrics chart (served under /metrics/, SVG kept at its stable
// root path because the README embeds it).

const config: Config = {
  title: 'DuckDB MSSQL Extension',
  tagline: 'SQL Server and Azure SQL from DuckDB — native TDS, no ODBC, built for speed.',
  favicon: 'img/favicon.ico',

  url: 'https://hugr-lab.github.io',
  baseUrl: '/mssql-extension/',
  trailingSlash: true,

  organizationName: 'hugr-lab',
  projectName: 'mssql-extension',

  onBrokenLinks: 'throw',
  onBrokenMarkdownLinks: 'warn',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: './sidebars.ts',
          // Docs ARE the site: /mssql-extension/<page>/
          routeBasePath: '/',
          // VERSIONING CONTRACT: at each release, run
          //   npm run docusaurus docs:version <X.Y.Z>
          // in website/ and commit the snapshot. Docusaurus then serves the
          // latest RELEASED version at the root (the default) and the live
          // docs/ tree as "Next" under /next/ with an "unreleased" banner —
          // so in-progress docs for the coming release are reachable but
          // never the landing default. Until the first snapshot exists,
          // current docs serve at the root with no dropdown entry to switch.
          editUrl: 'https://github.com/hugr-lab/mssql-extension/tree/main/website/',
          showLastUpdateTime: true,
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themes: ['@docusaurus/theme-mermaid'],

  markdown: {
    mermaid: true,
  },

  themeConfig: {
    metadata: [
      {name: 'keywords', content: 'DuckDB, SQL Server, Azure SQL, MSSQL, TDS, extension, BCP, bulk load, Fabric, Kerberos, Azure AD'},
      {name: 'description', content: 'DuckDB extension for Microsoft SQL Server and Azure SQL: attach databases, stream reads, bulk-load writes over a native TDS implementation.'},
    ],
    navbar: {
      title: 'MSSQL Extension',
      logo: {
        alt: 'Hugr Lab',
        src: 'img/logo-circle.svg',
        href: '/',
      },
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'docsSidebar',
          position: 'left',
          label: 'Docs',
        },
        {
          type: 'docsVersionDropdown',
          position: 'right',
        },
        {
          href: 'https://hugr-lab.github.io/mssql-extension/metrics/',
          label: 'Downloads',
          position: 'left',
        },
        {
          href: 'https://hugr-lab.github.io/',
          label: 'Hugr Lab',
          position: 'right',
        },
        {
          href: 'https://github.com/hugr-lab/mssql-extension',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    colorMode: {
      defaultMode: 'light',
      disableSwitch: true,
      respectPrefersColorScheme: false,
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Docs',
          items: [
            {label: 'Getting Started', to: '/getting-started/'},
            {label: 'Settings Reference', to: '/reference/settings/'},
            {label: 'Performance', to: '/performance/'},
          ],
        },
        {
          title: 'Community',
          items: [
            {label: 'GitHub', href: 'https://github.com/hugr-lab/mssql-extension'},
            {label: 'Issues', href: 'https://github.com/hugr-lab/mssql-extension/issues'},
            {label: 'DuckDB Community Extensions', href: 'https://duckdb.org/community_extensions/extensions/mssql.html'},
          ],
        },
        {
          title: 'Hugr Lab',
          items: [
            {label: 'Main site', href: 'https://hugr-lab.github.io/'},
            {label: 'Articles', to: '/articles/'},
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} Hugr Lab.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['sql', 'bash', 'powershell'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
