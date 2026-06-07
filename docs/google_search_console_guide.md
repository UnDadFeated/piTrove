# Guide: Submitting piTrove to Google Search Console

Follow these steps to submit your custom landing page/documentation site (`https://undadfeated.github.io/piTrove/`) directly to Google's Search Index.

## Prerequisites
1. Ensure your changes to the `docs/` folder are committed and pushed to the default/primary branch (so `https://undadfeated.github.io/piTrove/` is live and serving the new `index.html` page).
2. Ensure you have a Google account.

---

## Step-by-Step Setup

### Step 1: Add Site Property in Search Console
1. Visit the [Google Search Console](https://search.google.com/search-console/about) and sign in.
2. Click the property selector dropdown in the top-left corner and click **"Add property"**.
3. Under **"URL prefix"**, enter your live URL:
   ```
   https://undadfeated.github.io/piTrove/
   ```
4. Click **"Continue"**.

### Step 2: Site Verification
Google needs to verify that you own/control the repository hosting this site. You can do this in one of two ways:

#### Option A: HTML Tag (Recommended)
1. In the verification dialog, expand the **"HTML tag"** option.
2. Copy the meta tag provided (it looks like `<meta name="google-site-verification" content="..." />`).
3. Add this tag to the `<head>` of your [index.html](file:///p:/piTrove/docs/index.html) file, right below the other meta tags.
4. Commit and push the updated `index.html` file to your GitHub repository.
5. Wait 1-2 minutes for GitHub Pages to rebuild, then click **"Verify"** in Search Console.

#### Option B: HTML File Upload
1. In the verification dialog, download the verification file (e.g. `google12345.html`).
2. Place this file inside the `docs/` folder of your local repository.
3. Commit and push this file to your GitHub repository.
4. Once live, click **"Verify"** in Search Console.

### Step 3: Submit the Sitemap
Once verified, submit the sitemap to tell Google about all paths/changes:
1. In the Search Console left-hand sidebar, select **"Sitemaps"**.
2. Under **"Add a new sitemap"**, type:
   ```
   sitemap.xml
   ```
3. Click **"Submit"**.

Google will crawl your sitemap and index your premium landing page within a few days!
