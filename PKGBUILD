# Maintainer: Your Name <your-email@example.com>
pkgname=baresip-qt
pkgver=4.10.0_qt1
pkgrel=1
pkgdesc="Baresip bundled with custom native Qt applet"
arch=('x86_64')
url="https://github.com"
license=('BSD-3-Clause')

depends=('qt6-base' 'libre' 'openssl' 'opus')
makedepends=('cmake' 'git')

provides=('baresip')
conflicts=('baresip')

# Build from the local source tree (this repository).
source=("git+file://${PWD}#commit=f2eba89c")
sha256sums=('SKIP')

build() {
  # No -DMODULES override: build all modules whose deps are available
  # (each module's CMakeLists auto-skips when its deps are missing).
  cmake -B build -S "${srcdir}/${pkgname}" \
    -DCMAKE_BUILD_TYPE=None \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build build
}

package() {
  cmake --install build --prefix="${pkgdir}/usr"
}
