# shellcheck shell=bash
# Repository and checksum are filled by packaging/update-pkgbuild.py.
# They intentionally remain invalid until a real GitHub release exists.
pkgname=snipline
pkgver=0.1.0
pkgrel=1
pkgdesc='Wayland screenshot, recording, QR and OCR selector'
arch=('x86_64')
url='https://github.com/OWNER/REPOSITORY'
license=('MIT')
depends=('curl' 
  'gcc-libs' 'glibc'
  'qt6-base>=6.5' 'qt6-declarative' 'qt6-wayland' 'qt6-svg'
  'layer-shell-qt' 'wayland' 'grim'
  'gstreamer>=1.20' 'gst-plugins-base-libs' 'gst-plugins-base' 'gst-plugins-good'
  'gst-plugins-ugly' 'gst-libav' 'libpulse' 'zxing-cpp>=2.0'
)
makedepends=('cmake>=3.24' 'ninja' 'pkgconf' 'wayland-protocols>=1.41' 'hyprland-protocols>=0.8')
optdepends=(
  'tesseract: OCR recognition'
  'tesseract-data-eng: English OCR model; install other language packages as needed'
  'satty: optional screenshot editor'
  'ffmpeg: GIF export'
  'gst-plugins-bad: optional hardware encoders and additional codecs'
  'hyprland: supported Wayland compositor'
  'niri: supported Wayland compositor with compatible capture protocols'
)
options=('!debug')
source=("$pkgname-$pkgver.tar.gz::$url/releases/download/v$pkgver/$pkgname-$pkgver.tar.gz")
sha256sums=('REPLACE_WITH_RELEASE_SHA256')

build() {
  cmake -S "$srcdir/$pkgname-$pkgver" -B "$srcdir/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF
  cmake --build "$srcdir/build"
}

package() {
  DESTDIR="$pkgdir" cmake --install "$srcdir/build"
}
