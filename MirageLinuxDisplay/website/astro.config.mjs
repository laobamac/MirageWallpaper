// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import sitemap from '@astrojs/sitemap';

// Deployed to GitHub Pages as a project site at
// https://elysia-best.github.io/MirageLinuxDisplay/. The repository is
// elysia-best/MirageLinuxDisplay, so the site lives under the /MirageLinuxDisplay/
// base path; `site` + `base` produce correct absolute URLs and asset paths.
// Override locally with SITE / BASE.
const SITE = process.env.SITE ?? 'https://elysia-best.github.io';
const BASE = process.env.BASE ?? '/MirageLinuxDisplay/';

export default defineConfig({
  site: SITE,
  base: BASE,
  trailingSlash: 'ignore',
  integrations: [
    starlight({
      title: 'MirageLinuxDisplay',
      description:
        '为 MirageWallpaper 打造的 Linux 桌面环境显示集成层：mirage-display-v1 协议、C ABI 消费/生产库与路由核心。',
      tagline: '把 MirageWallpaper 的帧呈现在每一块 Linux 桌面上',
      logo: {
        light: './src/assets/logo.svg',
        dark: './src/assets/logo.svg',
        replacesTitle: false,
      },
      favicon: '/favicon.svg',
      customCss: ['./src/styles/theme.css', './src/styles/landing.css'],
      social: [
        {
          icon: 'github',
          label: 'GitHub',
          href: 'https://github.com/elysia-best/MirageLinuxDisplay',
        },
      ],
      defaultLocale: 'root',
      locales: {
        root: {
          label: '简体中文',
          lang: 'zh-CN',
        },
      },
      editLink: {
        baseUrl: 'https://github.com/elysia-best/MirageLinuxDisplay/edit/master/website/',
      },
      lastUpdated: true,
      pagination: true,
      sidebar: [
        {
          label: '开始使用',
          items: [
            { slug: 'guides/introduction' },
            { slug: 'guides/architecture' },
            { slug: 'guides/build' },
          ],
        },
        {
          label: '协议参考',
          items: [
            { slug: 'protocol/overview' },
            { slug: 'protocol/handshake' },
            { slug: 'protocol/buffers' },
            { slug: 'protocol/input' },
            { slug: 'protocol/errors' },
          ],
        },
        {
          label: '开发者指南',
          items: [
            { slug: 'dev/consumer' },
            { slug: 'dev/producer' },
            { slug: 'dev/broker' },
            { slug: 'dev/gpu' },
            { slug: 'dev/examples' },
            { slug: 'dev/tests' },
          ],
        },
        {
          label: '桌面环境适配',
          items: [
            { slug: 'adapters/boundary' },
            { slug: 'adapters/kde' },
            { slug: 'adapters/planned' },
          ],
        },
        {
          label: '参考',
          items: [
            { slug: 'reference/abi' },
            { slug: 'reference/faq' },
            { slug: 'reference/troubleshooting' },
          ],
        },
      ],
    }),
    sitemap(),
  ],
});
