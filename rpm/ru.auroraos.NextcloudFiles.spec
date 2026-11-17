Name:       ru.auroraos.NextcloudFiles
Summary:    AuroOo file browser
Version:    1.0
Release:    1
Group:      Qt/Qt
License:    Proprietary
URL:        https://nextcloud.com/
Source0:    %{name}-%{version}.tar.bz2

Requires:   sailfishsilica-qt5 >= 0.10.9
BuildRequires:  pkgconfig(auroraapp)
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(runtime-manager-qt5)
BuildRequires:  qt5-qttools-linguist

%description
Native Aurora OS client for listing files in a Nextcloud account.

%prep
%autosetup

%build
%cmake -GNinja
%ninja_build

%install
%ninja_install

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%defattr(644,root,root,-)
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
