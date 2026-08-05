# workspace — the Workspace Profile domain

The **workspace** domain owns the environment-level state a user describes about
themselves and their data, so the product can suggest what they could build. Its
namespace is `zetta::workspace` (`include/zetta/workspace/`,
`src/core/workspace/`).

| Page | What it covers |
|---|---|
| [workspace-profile.md](workspace-profile.md) | The **Workspace Profile** replicated entity (#886): owner, role, daily-work description, approved source refs; the `profile:<uuid>` CRDT key, the `system.workspace_profiles` view, and the governed create/get/update capabilities. |

**Where it fits.** The Workspace Profile is the input to the *Discover
Opportunities* pathway (the home's second door, beside *Create a Solution*): the
**Auto-Suggest** model ([#554](https://github.com/oramasearch/zetta/issues/554))
reads a profile to rank Opportunities the environment can support. The profile is
real replicated state today; until Auto-Suggest ships, the Opportunities surface
stays honestly in-training — the profile is built and stored, the ranking is not
yet live.
