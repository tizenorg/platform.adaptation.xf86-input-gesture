#sbs-git:slp/pkgs/xorg/driver/xserver-xorg-input-gesture xorg-x11-drv-gesture 0.1.0 fb1092a8ea453d658b38d5c28e67a58462d7c931
Name:	xorg-x11-drv-gesture
Summary:    X.Org X server -- Xserver gesture driver
Version: 0.1.2
Release:    2
Group:      System/X Hardware Support
License:    MIT
Source0:    %{name}-%{version}.tar.gz
BuildRequires:  pkgconfig(xorg-server)
BuildRequires:  xorg-x11-proto-gesture
BuildRequires:  pkgconfig(xproto)
BuildRequires:  pkgconfig(inputproto)
BuildRequires:  pkgconfig(resourceproto)
BuildRequires:  pkgconfig(xorg-macros)

%description
 This package provides the driver for recognizing gesture(s) using button
and motion events inside X server.


%package devel
Summary:    Development files for xorg gesture driver
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}

%description devel
xorg-x11-drv-gesture development files


%prep
%setup -q

%build

autoreconf -vfi
./configure --prefix=/usr --mandir=/usr/share/man --infodir=/usr/share/info CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS"
#./configure --prefix=/usr --mandir=/usr/share/man --infodir=/usr/share/info CFLAGS="$CFLAGS -D__DETAIL_DEBUG__ -D__DEBUG_EVENT_HANDLER__ " LDFLAGS="$LDFLAGS"

make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/usr/share/license
cp -af COPYING %{buildroot}/usr/share/license/%{name}
%make_install

%remove_docs

%files
%{_libdir}/xorg/modules/input/gesture_drv.so
/usr/share/license/%{name}

%files devel
%{_libdir}/pkgconfig/xorg-gesture.pc
