# Maintainer: Your Name <your-email@example.com>
pkgname=baresip-qt
pkgver=4.10.0_qt1
pkgrel=1
pkgdesc="Baresip bundled with custom native Qt applet"
arch=('x86_64')
url="https://github.com"
license=('BSD-3-Clause')

depends=('qt6-base' 'libre' 'openssl' 'opus')
makedepends=('cmake')

provides=('baresip')
conflicts=('baresip')

# Point directly to the stable release archive you just generated on GitHub
source=("https://github.com/archive/refs/tags/v4.10.0-qt1.tar.gz")

# Crucial for security! This ensures nobody tampers with your download file
sha256sums=('GENERATE_THIS_IN_THE_NEXT_STEP')

build() {
  # GitHub extracts the folder as repository_name-tag_name
  cmake -B build -S "${srcdir}/baresip-4.10.0-qt1" \
    -DCMAKE_BUILD_TYPE=None \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DMODULES="qt_applet;account;contact;opus;srtp"
  cmake --build build
}

package() {
  cmake --install build --prefix="${pkgdir}/usr"
}

