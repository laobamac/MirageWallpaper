// @ts-check
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import sitemap from '@astrojs/sitemap';

// Deployed to GitHub Pages at https://laobamac.github.io/MirageWallpaper/
// `site` + `base` produce correct absolute URLs and asset paths under the
// project sub-path. Override locally with SITE / BASE env vars if needed.
const SITE = process.env.SITE ?? 'https://laobamac.github.io';
const BASE = process.env.BASE ?? '/MirageWallpaper/';

export default defineConfig({
  site: SITE,
  base: BASE,
  trailingSlash: 'ignore',
  integrations: [
    starlight({
      title: 'Mirage',
      description: '面向 macOS 的原生动态壁纸管理器与 Wallpaper Engine 兼容运行时。',
      tagline: '让桌面动起来',
      logo: {
        light: './src/assets/logo.png',
        dark: './src/assets/logo.png',
        replacesTitle: false,
      },
      favicon: '/favicon.png',
      customCss: [
        './src/styles/theme.css',
        './src/styles/landing.css',
      ],
      social: [
        {
          icon: 'github',
          label: 'GitHub',
          href: 'https://github.com/laobamac/MirageWallpaper',
        },
      ],
      defaultLocale: 'root',
      locales: {
        root: {
          label: '简体中文',
          lang: 'zh-CN',
        },
        en: {
          label: 'English',
          lang: 'en',
        },
      },
      editLink: {
        baseUrl: 'https://github.com/laobamac/MirageWallpaper/edit/main/website/',
      },
      lastUpdated: true,
      pagination: true,
      sidebar: [
        {
          label: '开始使用',
          translations: { en: 'Getting Started' },
          items: [
            { slug: 'guides/introduction' },
            { slug: 'guides/requirements' },
            { slug: 'guides/install' },
            { slug: 'guides/interface' },
          ],
        },
        {
          label: '使用壁纸',
          translations: { en: 'Using Wallpapers' },
          items: [
            { slug: 'wallpapers/library' },
            { slug: 'wallpapers/apply' },
            { slug: 'wallpapers/playback' },
            { slug: 'wallpapers/displays' },
            { slug: 'wallpapers/menubar' },
          ],
        },
        {
          label: '导入与格式',
          translations: { en: 'Import & Format' },
          items: [
            { slug: 'formats/wallpaper-types' },
            { slug: 'formats/import' },
            { slug: 'formats/video-convert' },
            { slug: 'formats/project-json' },
          ],
        },
        {
          label: 'Steam 创意工坊',
          translations: { en: 'Steam Workshop' },
          items: [
            { slug: 'workshop/overview' },
            { slug: 'workshop/setup-wizard' },
            { slug: 'workshop/api-key' },
            { slug: 'workshop/steamcmd' },
            { slug: 'workshop/login' },
            { slug: 'workshop/browse' },
            { slug: 'workshop/download' },
            { slug: 'workshop/presets' },
            { slug: 'workshop/troubleshooting' },
          ],
        },
        {
          label: '屏保',
          translations: { en: 'Screen Saver' },
          items: [{ slug: 'screensaver/overview' }],
        },
        {
          label: '设置详解',
          translations: { en: 'Settings' },
          items: [
            { slug: 'settings/overview' },
            { slug: 'settings/performance' },
            { slug: 'settings/general' },
            { slug: 'settings/plugins' },
            { slug: 'settings/screensaver' },
            { slug: 'settings/updates' },
            { slug: 'settings/web-safety' },
          ],
        },
        {
          label: '架构与进阶',
          translations: { en: 'Architecture & Advanced' },
          items: [
            { slug: 'advanced/architecture' },
            { slug: 'advanced/data-directories' },
            { slug: 'advanced/build' },
            { slug: 'advanced/debug-renderers' },
            { slug: 'advanced/ci' },
          ],
        },
        {
          label: '参考',
          translations: { en: 'Reference' },
          items: [
            { slug: 'reference/faq' },
            { slug: 'reference/troubleshooting' },
            { slug: 'reference/privacy' },
            { slug: 'reference/community' },
          ],
        },
      ],
    }),
    sitemap(),
  ],
});
