Option Explicit

Dim ModIn(10)
ModIn(0) = 1

Class FlipperPolarity
    Private Enabled

    Private Sub Class_Initialize()
        Enabled = True
    End Sub

    Public Sub Slingshot(aBall)
        Dim BallPos
        BallPos = 0.5

        'Velocity angle correction
        Dim Angle, RotVxVy
        If Not IsEmpty(ModIn(0)) Then
            ' Dim moved above
            Angle = 45
            RotVxVy = Array(1.0, 2.0)
            If Enabled Then aBall.Velx = RotVxVy(0)
            If Enabled Then aBall.Vely = RotVxVy(1)
        End If
    End Sub
End Class

Class Ball
    Public Velx
    Public Vely
End Class

Dim testBall
Set testBall = New Ball
testBall.Velx = 0
testBall.Vely = 0

Dim fp
Set fp = New FlipperPolarity
fp.Slingshot testBall
