import React, {type ReactNode} from 'react';
import SiteMetadata from '@theme-original/SiteMetadata';
import Head from '@docusaurus/Head';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import {useLocation} from '@docusaurus/router';

// While the hugr-lab.com copy of these docs exists, the GitHub Pages copy names
// it as the canonical page (customFields.canonicalBase, see docusaurus.config.ts),
// so search engines index hugr-lab.com and the github.io links keep working.
// A wrapper, not an eject: this Head comes after the original one, so its og:url
// and canonical are the ones left in the page -- in the built HTML and after
// hydration (a sed over the built HTML would be undone by react-helmet).
export default function SiteMetadataWrapper(): ReactNode {
  const {
    siteConfig: {baseUrl, customFields},
  } = useDocusaurusContext();
  const {pathname} = useLocation();
  const base = customFields?.canonicalBase as string | undefined;
  if (!base) {
    return <SiteMetadata />;
  }
  const url = base + (pathname.startsWith(baseUrl) ? pathname.slice(baseUrl.length) : '');
  return (
    <>
      <SiteMetadata />
      <Head>
        <meta property="og:url" content={url} />
        <link rel="canonical" href={url} />
      </Head>
    </>
  );
}
