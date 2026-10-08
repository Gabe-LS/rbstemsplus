# Security

RB Stems Plus changes files inside rekordbox and runs a few steps as an administrator, so
security problems matter a lot. Thanks for taking the time to report one.

## How to report a problem privately

Please don't open a public issue for a security problem. Instead:

1. Go to the [Security tab](https://github.com/Gabe-LS/rbstemsplus/security) of this repository.
2. Click **Report a vulnerability**.
3. Say what you found, how to make it happen, and what someone could do with it.

Only you and the maintainer can see the report. There is no email address for this: GitHub's
private reporting is the only channel.

## What counts

- **The app**, RB Stems Plus itself, and its watcher.
- **The install command** (`bootstrap.sh`) and what it downloads.
- **The steps that run as an administrator** (the root scripts).
- **The Stems Cache bridge**, the library that sits between rekordbox and its stems model.
- **Release signing**: anything that would let someone make the app or the install command
  accept files that weren't signed by the release keys.

Problems in rekordbox itself belong to AlphaTheta, not here. RB Stems Plus isn't affiliated with
AlphaTheta or Pioneer DJ.

## What happens next

- You get a first answer as soon as I can, usually within two weeks. RB Stems Plus is a
  one-person project.
- Once the problem is confirmed, a fix goes into a new signed release as soon as it's ready. The
  release notes say what was fixed.
- You're credited in the release notes if you want to be. Just say so in your report.

Please give a reasonable amount of time for a fix before talking about the problem in public.
