#!/usr/bin/perl

#
# Script to take assets from squeezeplay/assets/MasterIcons directory and resize them into correct size(s)
#
# 1. runs a convert command to each file from @iconConvertList from the MasterIcons file to correct w x h dimensions for the skin (or skins), saves output to a temp folder
# 2. does a diff between the new file and the one currently in iconsResized with same name
# 3. if different (or new), copies it into iconsResized
# 4. runs svk status, uses output to create a shell script for doing the necessary svk commands
#
# bklaas 03.09

use strict;
use File::Next;
use File::Basename;
use File::Copy;

my $resizedIconDir = "images/IconsResized";
my $assetDir = "../../../../../assets/MasterIcons";
my $convertCommand = "/opt/local/bin/convert";

# define skins and dimensions for each
my $resize = { 
		'touch'	=>	41,
		'remote' =>	64,
};


# only convert these icons
my @iconConvertList = (qw/ 
icon_album_noart.png
icon_app_guide.png
icon_boom.png
icon_controller.png
icon_ethernet.png
icon_fab4.png
icon_internet_radio.png
icon_mymusic.png
icon_power_off2.png
icon_receiver.png
icon_region_americas.png
icon_region_other.png
icon_SB1n2.png
icon_SB3.png
icon_settings.png
icon_slimp3.png
icon_softsqueeze.png
icon_squeezeplay.png
icon_transporter.png
icon_wireless.png
/);

my $existingImages = get_assets($resizedIconDir);
remove_images($existingImages);

# convert the files
convert_files();

# create svk file
create_svk_file();

sub convert_files {
	#for my $file (sort keys %$assets) {
	for my $f (@iconConvertList) {
		my $file = $assetDir . "/" . $f;
		for my $skin (sort keys %$resize) {
			my $size = $resize->{$skin};
			my $basename = fileparse($file, qr/\.[^.]*/);
			my $filename = $resizedIconDir . "/" . $basename . "_" . $skin . ".png";
			my $resizeMe = "$convertCommand -geometry ${size}x${size} $file $filename";
			open(PROG, "$resizeMe |") or die "Could not run convert command: $!";
			print STDOUT "$resizeMe\n";
			while(<PROG>) {
			}
			close(PROG);
		}
	}
}

sub remove_images {
	my $images = shift;
	for my $image (keys %$images) {
		print "REMOVING $image\n";
		my $success = unlink $image;
		if ($success != 1) {
			print STDERR "There was a problem removing $image\n";
		}
	}
}

sub create_svk_file {
	my $prog = "svk status $resizedIconDir";
	open(OUT, ">svkUpdate.bash");
	open(PROG, "$prog |");
	print OUT "#!/bin/bash\n\n";
	while(<PROG>) {
		s/^\?\s+/svk add /;
		s/^\!\s+/svk remove /;
		if (/^svk/) {
			print OUT $_;
		}
	}
	close(OUT);
	close(PROG);
}

sub get_assets {
	my $path = shift;
	my $files = File::Next::files($path);
	my %return;
	while ( defined ( my $file = $files->() ) ) {
		my ($name,$dir,$suffix) = fileparse($file);
		if ($path eq $assetDir && $dir !~ /Apps/ && $dir !~ /Music_Library/) {
			$return{$file}++ if $file =~ /\.png$/;
		} elsif ($path ne $assetDir) {
			$return{$file}++ if $file =~ /\.png$/ && $file !~ /UNOFFICIAL/;
		}
	}
	return \%return;
}

sub copy_assets {
	my $assets = shift;
	for my $image (@$assets) {
		my $newImage = $image;
		$newImage =~ s/$assetDir/$resizedIconDir/;
		copy($image, $newImage);
	}
}
