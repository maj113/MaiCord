# Fedora COPR "make srpm" source: the project's Package settings are Source type "SCM", Clone URL
# https://github.com/maj113/MaiCord.git, Subdirectory packaging/rpm, Build SRPM with "make srpm", and the project setting
# "Enable internet access during builds" off (only this step needs the network: the build itself installs the tarball).
# COPR runs `make -f .copr/Makefile srpm outdir=... spec=...` in a Fedora chroot: it downloads the release tarball the spec
# names, checks it against the release's .sha256 file and makes the source rpm.
.PHONY: srpm
srpm:
	dnf -y install rpm-build curl
	version=$$(rpmspec -q --srpm --qf '%{version}' $(spec)) && \
	name=maicord-$$version-linux-x86_64.tar.gz && \
	url=https://github.com/maj113/MaiCord/releases/download/v$$version && \
	curl -fsSL --retry 3 -o $$name $$url/$$name && curl -fsSL --retry 3 -o $$name.sha256 $$url/$$name.sha256 && \
	sha256sum -c $$name.sha256 && \
	rpmbuild -bs --define "_sourcedir $(CURDIR)" --define "_srcrpmdir $(outdir)" $(spec)
